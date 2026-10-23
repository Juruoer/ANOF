#pragma once
#include <boost/test/unit_test.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <set>
#include <sstream>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
namespace anof::testing::cli {
using Json=nlohmann::json;
struct Temp {
    std::filesystem::path path;
    Temp() { char pattern[]="/tmp/anof-cli-XXXXXX"; const auto* p=mkdtemp(pattern); if(!p) throw std::runtime_error("temporary root failed"); path=p; }
    ~Temp() { std::error_code ignored; std::filesystem::remove_all(path,ignored); }
};
inline void file(const std::filesystem::path& path,const std::string& content) { std::ofstream out(path); out<<content; if(!out) throw std::runtime_error("fixture write failed"); }
inline Json jsonFile(const std::filesystem::path& path) { std::ifstream input(path); return Json::parse(input); }
struct Process {
    pid_t pid=-1;
    int output=-1,error=-1,master=-1,slave=-1;
    std::optional<int> status;
    std::string out,err,tty;
    termios original{};
    std::string diagnosticCommand;
    std::string failureSummary() const {
        std::ostringstream text;
        text << "command=" << diagnosticCommand << " stdout_bytes=" << out.size() << " stderr_bytes=" << err.size();
        // 只输出结构化分类，不回显message、原始响应、凭据、参数值或PTY内容。
        auto value=Json::parse(err,nullptr,false);
        if(value.is_object() && value.contains("error") && value["error"].is_object()) {
            for(const auto* key:{"code","request_id","remote_outcome","local_state"}) {
                auto it=value["error"].find(key);
                if(it==value["error"].end() || !it->is_string()) continue;
                const auto word=it->get<std::string>();
                if(word.size()<=80 && word.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")==std::string::npos)
                    text << " " << key << "=" << word;
            }
            auto it=value["error"].find("http_status");
            if(it!=value["error"].end() && it->is_number_integer()) text << " http_status=" << *it;
        } else text << " stderr_format=non_json";
        return text.str();
    }
    Process(std::vector<std::string> args,bool terminal=false,bool closedOutput=false) {
        static const std::set<std::string> verbs{"init","serve","profile","login","register","logout","me","spaces","ls","stat","mkdir","invite","account","user","quota","upload","download","storage","add","list","show","create","revoke","rename","password","disable","enable","set","resume","complete","cancel","mv","cp","rm","search","trash","restore","purge"};
        for(const auto& arg:args) if(verbs.count(arg)) diagnosticCommand+=(diagnosticCommand.empty()?"":" ")+arg;
        args.insert(args.begin(),ANOF_EXECUTABLE); std::vector<char*> argv; for(auto& arg:args) argv.push_back(arg.data()); argv.push_back(nullptr);
        int stdoutPipe[2],stderrPipe[2];
        if(pipe2(stdoutPipe,O_CLOEXEC)||pipe2(stderrPipe,O_CLOEXEC)) throw std::runtime_error("pipe failed");
        if(terminal) {
            master=posix_openpt(O_RDWR|O_NOCTTY|O_CLOEXEC|O_NONBLOCK);
            if(master<0||grantpt(master)||unlockpt(master)) throw std::runtime_error("PTY failed");
            slave=open(ptsname(master),O_RDWR|O_NOCTTY|O_CLOEXEC); if(slave<0||tcgetattr(slave,&original)) throw std::runtime_error("PTY slave failed");
        }
        pid=fork();
        if(pid==0) {
            alarm(55); const rlimit limit{0,0}; setrlimit(RLIMIT_CORE,&limit);
            if(setsid()<0) _exit(120);
            if(terminal&&ioctl(slave,TIOCSCTTY,0)!=0) _exit(121);
            int empty=open("/dev/null",O_RDONLY); if(empty<0||dup2(empty,STDIN_FILENO)<0) _exit(122); close(empty);
            if(dup2(stdoutPipe[1],STDOUT_FILENO)<0||dup2(stderrPipe[1],STDERR_FILENO)<0) _exit(123);
            close(stdoutPipe[0]); close(stdoutPipe[1]); close(stderrPipe[0]); close(stderrPipe[1]);
            if(master>=0) close(master);
            if(slave>=0) close(slave);
            execv(argv[0],argv.data()); _exit(124);
        }
        close(stdoutPipe[1]);close(stderrPipe[1]);
        if(pid<0) throw std::runtime_error("fork failed");
        output=stdoutPipe[0]; error=stderrPipe[0];
        if(closedOutput) { close(output); output=-1; }
        for(int fd:{output,error}) if(fd>=0&&fcntl(fd,F_SETFL,O_NONBLOCK)<0) throw std::runtime_error("pipe flags failed");
    }
    ~Process() {
        // 保留服务子进程按需诊断；固定字段日志不包含请求正文或凭据。
        try {
            pump();
            std::istringstream lines(err);std::string line;unsigned emitted=0;
            while(std::getline(lines,line) && emitted<16)
                if(line.rfind("ANOF_TRANSFER_DIAGNOSTIC ",0)==0 && line.size()<1024 &&
                   line.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-=. ")==std::string::npos) {
                    std::cerr << line << '\n';++emitted;
                }
        } catch(...) {}
        if(pid>0&&!status) { kill(pid,SIGKILL); while(waitpid(pid,nullptr,0)<0&&errno==EINTR) {} }
        for(int fd:{output,error,master,slave}) if(fd>=0) close(fd);
    }
    void pump() {
        auto readAll=[](int fd,std::string& text) { if(fd<0) return; char buffer[4096]; for(;;) { auto n=read(fd,buffer,sizeof buffer); if(n<=0) break; text.append(buffer,static_cast<std::size_t>(n)); if(text.size()>1024*1024) throw std::runtime_error("test output budget"); } };
        readAll(output,out); readAll(error,err); readAll(master,tty);
        if(!status) { int value=0; auto result=waitpid(pid,&value,WNOHANG); if(result==pid) {status=value; readAll(output,out);readAll(error,err);readAll(master,tty);} }
    }
    void until(const std::function<bool()>& condition) {
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
        do { pump(); if(condition()) return; pollfd p{error,POLLIN,0}; (void)poll(&p,1,10); } while(std::chrono::steady_clock::now()<deadline);
        throw std::runtime_error("client process deadline");
    }
    void input(const std::string& prompt,const std::string& secret) {
        until([&]{return tty.find(prompt)!=std::string::npos||status.has_value();}); BOOST_REQUIRE(!status);
        termios state{}; BOOST_REQUIRE_EQUAL(tcgetattr(slave,&state),0); BOOST_CHECK((state.c_lflag&(ECHO|ECHONL))==0);
        const auto line=secret+"\n"; BOOST_REQUIRE_EQUAL(write(master,line.data(),line.size()),static_cast<ssize_t>(line.size()));
    }
    void finish(int code) {
        until([&]{return status.has_value();}); BOOST_REQUIRE(WIFEXITED(*status)); BOOST_CHECK_MESSAGE(WEXITSTATUS(*status)==code, failureSummary() << " expected_exit=" << code << " actual_exit=" << WEXITSTATUS(*status));
        if(slave>=0) {termios state{}; BOOST_REQUIRE_EQUAL(tcgetattr(slave,&state),0); BOOST_CHECK_EQUAL(state.c_lflag,original.c_lflag);}
    }
    Json success() { finish(0); BOOST_CHECK_MESSAGE(err.empty(),failureSummary()); BOOST_REQUIRE_MESSAGE(!out.empty(),failureSummary()); BOOST_CHECK_EQUAL(std::count(out.begin(),out.end(),'\n'),1); auto j=Json::parse(out); BOOST_REQUIRE(j.at("ok").get<bool>()); return j; }
    Json failure(int code) { finish(code); BOOST_CHECK(out.empty()); BOOST_REQUIRE(!err.empty()); BOOST_CHECK_EQUAL(std::count(err.begin(),err.end(),'\n'),1); auto j=Json::parse(err); BOOST_REQUIRE(!j.at("ok").get<bool>()); return j.at("error"); }
    unsigned port() { const std::string prefix="ANOF_READY address=127.0.0.1 port="; until([&]{return out.find(prefix)!=std::string::npos||status.has_value();}); BOOST_REQUIRE(!status); return static_cast<unsigned>(std::stoul(out.substr(out.find(prefix)+prefix.size()))); }
    void stop() { BOOST_REQUIRE_EQUAL(kill(pid,SIGTERM),0); finish(0); }
};
inline std::vector<std::string> command(const Temp& temp,const std::string& profile,std::vector<std::string> args,bool json=true) {
    std::vector<std::string> result{"--config-dir",(temp.path/"config").string()}; if(json) result.push_back("--json");
    if(!profile.empty()) {result.push_back("--profile");result.push_back(profile);} result.insert(result.end(),args.begin(),args.end()); return result;
}
inline Json run(const Temp& temp,const std::string& profile,std::vector<std::string> args) {Process process(command(temp,profile,std::move(args)));return process.success();}
inline void login(const Temp& temp,const std::string& profile,const std::string& name,const std::string& password) {
    Process p(command(temp,profile,{"login","--login",name}),true);p.input("Password: ",password);auto j=p.success();
    BOOST_CHECK(j["data"]["credential_saved"].get<bool>()); BOOST_CHECK(!j["data"].contains("access_token")); BOOST_CHECK(p.tty.find("Confirm")==std::string::npos); BOOST_CHECK(p.tty.find(password)==std::string::npos); BOOST_CHECK(p.out.find(password)==std::string::npos);
}
}  // namespace anof::testing::cli
