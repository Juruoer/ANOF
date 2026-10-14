#define BOOST_TEST_MODULE AnofClientHttp
#include <boost/test/unit_test.hpp>
#include <anof/client/CurlHttpTransport.h>
#include <anof/client/Protocol.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>
using namespace anof;
using namespace anof::client;
namespace {
using namespace std::chrono_literals;
struct Socket {
    int fd = -1;
    explicit Socket(int value) : fd(value) { if (fd < 0) throw std::runtime_error("test socket failed"); }
    ~Socket() { close(fd); }
};
struct Server {
    Socket listener{socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0)};
    std::thread worker;
    unsigned port;
    std::string received;
    std::exception_ptr error;
    std::atomic<unsigned> connections{0};
    Server(std::function<void(int)> handler) {
        sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if (bind(listener.fd,reinterpret_cast<sockaddr*>(&address),sizeof address) || listen(listener.fd,4)) throw std::runtime_error("test bind failed");
        socklen_t size=sizeof address; if (getsockname(listener.fd,reinterpret_cast<sockaddr*>(&address),&size)) throw std::runtime_error("test address failed"); port=ntohs(address.sin_port);
        worker=std::thread([this,handler=std::move(handler)] {
            try {
                pollfd p{listener.fd,POLLIN,0}; if (poll(&p,1,3000)<=0) return;
                Socket peer(accept4(listener.fd,nullptr,nullptr,SOCK_CLOEXEC)); ++connections;
                timeval timeout{2,0}; setsockopt(peer.fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout); setsockopt(peer.fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
                handler(peer.fd);
            } catch (...) { error=std::current_exception(); }
        });
    }
    ~Server() { finish(); }
    void finish() { if (worker.joinable()) worker.join(); }
    Origin origin(bool tls=false) const { return Origin::parse(std::string(tls?"https":"http")+"://127.0.0.1:"+std::to_string(port),!tls).value(); }
};
std::string receive(int fd) {
    std::string result; char buffer[4096];
    while (result.size()<65536) { auto count=recv(fd,buffer,sizeof buffer,0); if(count<=0) break; result.append(buffer,static_cast<std::size_t>(count)); if(result.find("\r\n\r\n")!=std::string::npos) break; }
    return result;
}
void sendAll(int fd,std::string_view text) { while(!text.empty()) { auto count=send(fd,text.data(),text.size(),MSG_NOSIGNAL); if(count<=0) return; text.remove_prefix(static_cast<std::size_t>(count)); } }
HttpRequest request(Method method=Method::Get) { return {method,"/api/v1/users/me",SecureBuffer{},nullptr}; }
std::string ok() { return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nX-Request-ID: 40000000-0000-4000-8000-000000000001\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}"; }
struct ProxyEnvironment {
    std::map<std::string,std::optional<std::string>> old;
    ProxyEnvironment() {
        for (auto key : {"http_proxy","no_proxy","NO_PROXY"}) { auto* p=getenv(key); old[key]=p ? std::optional<std::string>(p) : std::nullopt; }
        setenv("http_proxy","http://127.0.0.1:1",1); setenv("no_proxy","",1); setenv("NO_PROXY","",1);
    }
    ~ProxyEnvironment() { for(const auto& entry:old) { if(entry.second) setenv(entry.first.c_str(),entry.second->c_str(),1); else unsetenv(entry.first.c_str()); } }
};
}
BOOST_AUTO_TEST_CASE(one_request_no_proxy_or_redirect_and_bounded_headers) {
    ProxyEnvironment proxy;
    std::string received;
    Server server([&](int fd){received=receive(fd); sendAll(fd,"HTTP/1.1 100 Continue\r\nX-Request-ID: ignored\r\n\r\n"+ok());});
    CurlHttpTransport transport; auto req=request(); auto token=SecretToken::session(std::string(43,'A')).value(); req.bearer=&token;
    auto result=transport.perform(server.origin(),req); server.finish(); BOOST_REQUIRE(result); BOOST_CHECK_EQUAL(result.value().status,200);
    BOOST_CHECK_EQUAL(result.value().headers.at("x-request-id"),"40000000-0000-4000-8000-000000000001");
    BOOST_CHECK(received.find("Authorization: Bearer "+std::string(43,'A'))!=std::string::npos); BOOST_CHECK(received.find("Accept-Encoding: identity")!=std::string::npos); BOOST_CHECK(received.find("Cookie:")==std::string::npos); BOOST_CHECK(received.find("X-Request-ID:")==std::string::npos);
    Server redirected([&](int fd){receive(fd); sendAll(fd,"HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/stolen\r\nContent-Length: 0\r\n\r\n");});
    result=transport.perform(redirected.origin(),req); redirected.finish(); BOOST_REQUIRE(result); BOOST_CHECK_EQUAL(result.value().status,302); BOOST_CHECK_EQUAL(redirected.connections,1);
    auto decoded=decodeResponse(Operation::Me,result.value()); BOOST_REQUIRE(!decoded); BOOST_CHECK(decoded.error().kind==ErrorKind::Protocol);
}
BOOST_AUTO_TEST_CASE(actual_budgets_duplicate_headers_and_truncation) {
    CurlHttpTransport transport(1000,1500);
    const std::vector<std::string> messages{
        "HTTP/1.1 200 OK\r\nContent-Length: 524289\r\n\r\n"+std::string(kResponseLimit+1,'x'),
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n80001\r\n"+std::string(kResponseLimit+1,'x')+"\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nX-Padding: "+std::string(kHeaderLimit,'x')+"\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\ncontent-type: application/json\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n{}"};
    for(const auto& message:messages) {
        Server server([&](int fd){receive(fd);sendAll(fd,message);}); auto result=transport.perform(server.origin(),request()); server.finish(); BOOST_REQUIRE(!result); BOOST_CHECK_EQUAL(result.error().exitCode(),4); BOOST_CHECK(result.error().remote==RemoteOutcome::Unknown);
        Server writer([&](int fd){receive(fd);sendAll(fd,message);}); result=transport.perform(writer.origin(),request(Method::Delete)); writer.finish(); BOOST_REQUIRE(!result); BOOST_CHECK_EQUAL(result.error().exitCode(),6); BOOST_CHECK_EQUAL(writer.connections,1);
    }
    std::string many; for(unsigned i=0;i<80;++i) many+="HTTP/1.1 100 Continue\r\nX-Padding: "+std::string(1000,'x')+"\r\n\r\n";
    Server server([&](int fd){receive(fd);sendAll(fd,many+ok());}); BOOST_CHECK(!transport.perform(server.origin(),request()));
}
BOOST_AUTO_TEST_CASE(deadlines_and_cancellation_do_not_retry) {
    CurlHttpTransport transport(100,100);
    Server server([](int fd){receive(fd);std::this_thread::sleep_for(200ms);});
    const auto start=std::chrono::steady_clock::now(); auto result=transport.perform(server.origin(),request(Method::Post)); server.finish();
    BOOST_REQUIRE(!result); BOOST_CHECK_EQUAL(result.error().exitCode(),6); BOOST_CHECK(std::chrono::steady_clock::now()-start<2s); BOOST_CHECK_EQUAL(server.connections,1);
    std::atomic_bool cancel{true}; result=transport.perform(Origin::parse("http://127.0.0.1",true).value(),request(Method::Post),&cancel); BOOST_REQUIRE(!result); BOOST_CHECK_EQUAL(result.error().exitCode(),130); BOOST_CHECK(result.error().remote==RemoteOutcome::NotSent);
    cancel=false; CurlHttpTransport longer(500,2000);
    Server interrupted([&](int fd){receive(fd);cancel=true;sendAll(fd,"HTTP/1.1 200 OK\r\nContent-Length: 20000\r\n\r\n"); for(unsigned i=0;i<10;++i) {sendAll(fd,std::string(1000,'x'));std::this_thread::sleep_for(20ms);}});
    result=longer.perform(interrupted.origin(),request(Method::Delete),&cancel); interrupted.finish(); BOOST_REQUIRE(!result); BOOST_CHECK_EQUAL(result.error().exitCode(),6);
}
BOOST_AUTO_TEST_CASE(untrusted_tls_certificate_rejected_before_bearer_http) {
    int peerStatus = -1;
    Server server([&](int fd) {
        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions) != 0) throw std::runtime_error("spawn setup failed");
        posix_spawn_file_actions_adddup2(&actions,fd,STDIN_FILENO);
        char python[] = ANOF_TEST_PYTHON;
        char script[] = ANOF_TEST_SOURCE_DIR "/TlsRejectServer.py";
        char* args[] = {python,script,nullptr};
        pid_t child = -1;
        const int result = posix_spawn(&child,python,&actions,nullptr,args,environ);
        posix_spawn_file_actions_destroy(&actions);
        if (result != 0) throw std::runtime_error("TLS peer spawn failed");
        while (waitpid(child,&peerStatus,0)<0 && errno==EINTR) {}
    });
    CurlHttpTransport transport(1000,1500); auto req=request(); auto token=SecretToken::session(std::string(43,'A')).value(); req.bearer=&token;
    auto result=transport.perform(server.origin(true),req); server.finish(); BOOST_REQUIRE(!server.error); BOOST_REQUIRE(!result); BOOST_CHECK_EQUAL(result.error().exitCode(),4);
    BOOST_REQUIRE(WIFEXITED(peerStatus)); BOOST_CHECK_EQUAL(WEXITSTATUS(peerStatus),0);
}
