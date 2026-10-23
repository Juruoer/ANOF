#include <anof/client/ApiClient.h>
#include <anof/client/TransferClient.h>
#include <anof/client/internal/TransferFiles.h>
#include <anof/domain/drive/DirectoryPolicy.h>
#include <anof/domain/transfer/TransferValues.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <chrono>
#include <thread>
namespace anof::client {
namespace {
using namespace transfer_internal;
using internal::decimal;
using internal::string;
template <class T>
T take(ClientResult<T> r) {
    if (!r) throw Failure{ r.error() };
    return std::move(r).value();
}
std::int64_t quantity(const Json& j, const char* key) { return decimal(string(j, key)); }
std::string field(const HttpResponse& r, const char* key) {
    auto it = r.headers.find(key);
    if (it == r.headers.end()) fail(ErrorKind::Protocol);
    return it->second;
}
bool sameInode(const Json& a, const Json& b) { return a.at("dev") == b.at("dev") && a.at("ino") == b.at("ino"); }
void interrupted(const std::atomic_bool* c) {
    if (c && c->load()) fail(ErrorKind::Interrupted);
}
}  // namespace
ClientResult<TransferReply> TransferClient::request(TransferOperation op, Method method, std::string path,
                                                    const nlohmann::json& body, unsigned limit,
                                                    std::optional<ConditionTag> tag) {
    if (!profile_.usable()) return failure(ErrorKind::LocalState);
    const auto& p = profile_.profile();
    if (!p.credential) return failure(ErrorKind::AuthenticationRequired);
    SecureBuffer bytes(kRequestLimit);
    if (!body.is_null() && !bytes.append(body.dump())) return failure(ErrorKind::InvalidArgument);
    auto result = http_.perform(
        p.server, { method, std::move(path), std::move(bytes), &p.credential->accessToken, std::move(tag) }, cancel_);
    if (!result) return result.error();
    return decodeTransfer(op, result.value(), limit);
}
ClientResult<TransferReply> TransferClient::show(UploadId id) {
    auto result=request(TransferOperation::Show, Method::Get, "/api/v1/uploads/" + id.toString());
    if(result && result.value().data()["id"]!=id.toString()) return failure(ErrorKind::Protocol,"REMOTE_BINDING_MISMATCH");
    return result;
}
ClientResult<TransferReply> TransferClient::list(PageRequest page, std::optional<OperationId> id) {
    auto query = pageQuery(page);
    if (!query) return query.error();
    if (id) query.value() += "&client_upload_id=" + id->toString();
    return request(TransferOperation::List, Method::Get, "/api/v1/uploads" + query.value(), nullptr, page.limit);
}
ClientResult<TransferReply> TransferClient::complete(UploadId id,
                                                     std::optional<std::pair<NodeId, std::string>> target) {
    Json body = Json::object();
    if (target) {
        if (!validateDirectoryName(target->second)) return failure(ErrorKind::InvalidArgument);
        body = { { "parent_id", target->first.toString() }, { "name", target->second } };
    }
    auto result=request(TransferOperation::Complete, Method::Post, "/api/v1/uploads/" + id.toString() + "/complete", body);
    if(result && (result.value().data()["id"]!=id.toString() || (target && (result.value().data()["parent_id"]!=target->first.toString() || result.value().data()["name"]!=target->second)))) return failure(ErrorKind::OutcomeUnknown,"REMOTE_BINDING_MISMATCH");
    return result;
}
ClientResult<TransferReply> TransferClient::cancel(UploadId id) {
    return request(TransferOperation::Cancel, Method::Delete, "/api/v1/uploads/" + id.toString());
}
ClientResult<TransferReply> TransferClient::storage() {
    return request(TransferOperation::Storage, Method::Get, "/api/v1/admin/storage");
}
ClientResult<TransferReply> TransferClient::setStorage(ByteCount budget, std::string tag) {
    auto parsed = ConditionTag::parse(tag);
    if (!parsed || tag.substr(0, 9) != "\"storage-") return failure(ErrorKind::InvalidArgument);
    return request(TransferOperation::SetStorage, Method::Put, "/api/v1/admin/storage",
                   { { "content_budget_bytes", std::to_string(budget.value()) } }, 200, parsed.value());
}
ClientResult<TransferResult> TransferClient::upload(std::string file, SpaceId space, NodeId parent, std::string name) {
    if (!validateDirectoryName(name) || !absolute(file)) return failure(ErrorKind::InvalidArgument);
    try {
        return workflow({}, false, std::move(file),
                        { { "space_id", space.toString() }, { "parent_id", parent.toString() }, { "name", name } });
    } catch (const Failure& e) {
        return e.error;
    } catch (...) {
        return failure(ErrorKind::LocalState);
    }
}
ClientResult<TransferResult> TransferClient::download(NodeId node, std::string output) {
    if (!absolute(output)) return failure(ErrorKind::InvalidArgument);
    try {
        return workflow({}, true, std::move(output), { { "node_id", node.toString() } });
    } catch (const Failure& e) {
        return e.error;
    } catch (...) {
        return failure(ErrorKind::LocalState);
    }
}
ClientResult<TransferResult> TransferClient::resume(std::string id, bool download) {
    try {
        return workflow(std::move(id), download, {}, {});
    } catch (const Failure& e) {
        return e.error;
    } catch (...) {
        return failure(ErrorKind::LocalState);
    }
}
TransferResult TransferClient::workflow(std::optional<std::string> existing, bool downloading, std::string file,
                                        Json target) {
    const auto& p = profile_.profile();
    if (!profile_.usable()) fail(ErrorKind::LocalState);
    if (!p.credential) fail(ErrorKind::AuthenticationRequired);
    Record record(directory_, p, existing);
    auto& r = record.data;
    RemoteOutcome remote = RemoteOutcome::NotSent;
    LocalState local = LocalState::TransferSaved;
    try {
        if (existing) {
            check(r.at("kind") == (downloading ? "download" : "upload"), "TRANSFER_KIND_MISMATCH");
            file = string(r, "file");
            target = r.at("target");
            check(absolute(file));
        } else {
            r["kind"] = downloading ? "download" : "upload";
            r["file"] = file;
            r["target"] = target;
        }
        auto me = take(ApiClient(http_, profile_, cancel_).me());
        check(me.data.id == p.credential->userId, "TRANSFER_BINDING_MISMATCH");
        const auto* bearer = &p.credential->accessToken;
        auto save = [&] { record.save(); };
        auto head = [&](const std::string& path, TransferOperation op) {
            TransferRequest req{ Method::Head, path, bearer, {}, 0, {}, {}, {} };
            auto response = take(binary_.transfer(p.server, req, cancel_));
            return take(decodeTransfer(op, response));
        };
        if (!downloading) {
            Json upload;
            if(existing) {
                std::string remoteId;
                if(r.at("upload_id").is_null()) {
                    auto key=OperationId::parse(string(r,"client_upload_id"));check(bool(key));
                    auto found=take(list({2,{}},key.value()));
                    if(found.data()["items"].size()!=1)fail(ErrorKind::OutcomeUnknown,"UPLOAD_CREATE_UNRESOLVED");
                    remoteId=string(found.data()["items"][0],"id");
                } else remoteId=string(r,"upload_id");
                auto id=UploadId::parse(remoteId);check(bool(id));
                auto confirmed=head("/api/v1/uploads/"+id.value().toString(),TransferOperation::UploadHead);
                upload=take(show(id.value())).data();
                check(confirmed.data()["length"]==upload["length"],"REMOTE_BINDING_MISMATCH");
            }
            Fd source;
            Json evidence;
            std::string digest;
            // 终态及后台发布只查询，不要求源文件仍然存在；仍需传输时才打开并重哈希。
            if(!existing || upload["state"]=="OPEN" || upload["state"]=="CREATING") {
                source=Fd(open(file.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
                evidence=identity(source.value);digest=hash(source.value,cancel_);
                check(identity(source.value)==evidence,"SOURCE_CHANGED");
                if(existing)check(r.at("source_identity")==evidence&&r.at("sha256")==digest,"SOURCE_CHANGED");
            }
            if(!existing) {
                r["source_identity"]=evidence;r["sha256"]=digest;r["length"]=evidence["size"];
                r["client_upload_id"]=uuid();r["upload_id"]=nullptr;r["phase"]="CREATING";save();
                Json body=target;body["length"]=r["length"];body["sha256"]=digest;body["client_upload_id"]=r["client_upload_id"];
                auto result=take(request(TransferOperation::Create,Method::Post,"/api/v1/uploads",body));
                remote=RemoteOutcome::ConfirmedSuccess;upload=result.data();
            }
            auto bound = [&](const Json& u, bool write = false) {
                if (u.at("client_upload_id") != r.at("client_upload_id") || u.at("space_id") != target.at("space_id") ||
                    u.at("parent_id") != target.at("parent_id") || u.at("name") != target.at("name") ||
                    u.at("length") != r.at("length") || u.at("expected_sha256") != r.at("sha256") ||
                    (!r.at("instance_id").is_null() && u.at("instance_id") != r.at("instance_id")) ||
                    (!r.at("upload_id").is_null() && u.at("id") != r.at("upload_id")))
                    fail(write ? ErrorKind::OutcomeUnknown : ErrorKind::Protocol, "REMOTE_BINDING_MISMATCH");
            };
            bound(upload,!existing);
            r["upload_id"] = upload["id"];
            r["instance_id"] = upload["instance_id"];
            r["phase"] = upload["state"];
            save();
            const auto id = UploadId::parse(string(upload, "id")).value();
            for (unsigned attempt = 0; attempt < 60 && upload["state"] == "CREATING"; ++attempt) {
                for (unsigned tick = 0; tick < 10; ++tick) {
                    interrupted(cancel_);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                upload = take(show(id)).data();
                bound(upload);
            }
            if (upload["state"] == "OPEN") {
                auto offset = quantity(upload, "offset"), length = quantity(upload, "length");
                while (offset < length) {
                    interrupted(cancel_);
                    check(identity(source.value) == evidence, "SOURCE_CHANGED");
                    auto count = std::min<std::int64_t>(upload["max_patch_bytes"].get<std::int64_t>(), length - offset);
                    std::int64_t read = 0;
                    TransferRequest req{ Method::Patch,
                                         "/api/v1/uploads/" + id.toString(),
                                         bearer,
                                         { { "Upload-Offset", std::to_string(offset) } },
                                         count,
                                         {},
                                         {},
                                         {} };
                    req.source = [&](char* data, std::size_t size) -> ClientResult<std::size_t> {
                        try {
                            ssize_t n;
                            do {
                                n = pread(source.value, data,
                                          static_cast<std::size_t>(
                                              std::min<std::int64_t>(static_cast<std::int64_t>(size), count - read)),
                                          offset + read);
                            } while (n < 0 && errno == EINTR);
                            check(n > 0 || read == count, "SOURCE_CHANGED");
                            read += n;
                            return static_cast<std::size_t>(n);
                        } catch (const Failure& e) {
                            return e.error;
                        }
                    };
                    auto response = take(binary_.transfer(p.server, req, cancel_));
                    auto patched = take(decodeTransfer(TransferOperation::Patch, response));
                    remote = RemoteOutcome::ConfirmedSuccess;
                    if (quantity(patched.data(), "offset") != offset + count)
                        fail(ErrorKind::OutcomeUnknown, "INVALID_UPLOAD_OFFSET");
                    check(identity(source.value) == evidence, "SOURCE_CHANGED");
                    offset += count;
                    r["durable_offset"] = std::to_string(offset);
                    save();
                }
                upload = take(complete(id)).data();
                bound(upload,true);
                remote = RemoteOutcome::ConfirmedSuccess;
                r["phase"] = upload["state"];
                save();
            }
            for (unsigned attempt = 0;
                 attempt < 60 && (upload["state"] == "VERIFYING" || upload["state"] == "COMMITTING" ||
                                  (upload["state"] == "READY" && upload["last_error"].is_null()));
                 ++attempt) {
                for (unsigned tick = 0; tick < 10; ++tick) {
                    interrupted(cancel_);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                upload = take(show(id)).data();
                bound(upload);
            }
            r["phase"] = upload["state"];
            r["result_node_id"] = upload["result_node_id"];
            save();
            if(upload["state"]=="COMPLETED" && !upload.at("result_deleted_at").is_null()) {
                remote=RemoteOutcome::ConfirmedSuccess;
                fail(ErrorKind::Application,"UPLOAD_RESULT_DELETED");
            }
            if (upload["state"] == "FAILED" || upload["state"] == "CANCELLED" || upload["state"] == "EXPIRED" ||
                upload["state"] == "READY")
                fail(ErrorKind::Application, "UPLOAD_REQUIRES_ACTION");
            return { { { "transfer_id", record.id() },
                       { "upload_id", id.toString() },
                       { "state", upload["state"] },
                       { "published", upload["state"] == "COMPLETED" },
                       { "node_id", upload["result_node_id"] } },
                     {},
                     RemoteOutcome::ConfirmedSuccess,
                     local };
        }
        auto node = NodeId::parse(string(target, "node_id"));
        check(bool(node));
        const auto url = "/api/v1/nodes/" + node.value().toString() + "/content";
        const auto metadata = head(url, TransferOperation::ContentHead);
        const auto length = quantity(metadata.data(), "length");
        const auto digest = string(metadata.data(), "sha256"), etag = string(metadata.data(), "etag");
        auto split = file.rfind('/');
        const auto name = file.substr(split + 1);
        auto parent = directory(split == 0 ? "/" : file.substr(0, split));
        const auto part = ".anof-download-" + record.id() + ".part";
        if (existing)
            check(r.at("length") == metadata.data()["length"] && r.at("sha256") == digest && r.at("etag") == etag,
                  "REPRESENTATION_CHANGED");
        else {
            r["length"] = std::to_string(length);
            r["sha256"] = digest;
            r["etag"] = etag;
            r["durable_offset"] = "0";
            r["part"] = part;
            r["phase"] = "PREPARING";
            save();
        }
        check(r.at("part") == part);
        Fd fd;
        const auto phase = string(r, "phase");
        if (phase == "PUBLISHING" || phase == "COMPLETED") {
            fd = Fd(openat(parent.value, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
            if (fd.value >= 0) {
                safeFile(fd.value);
                check(sameInode(identity(fd.value), r.at("part_identity")) &&
                          quantity(identity(fd.value), "size") == length && hash(fd.value, cancel_) == digest,
                      "PUBLISH_CONFLICT");
                check(fsync(parent.value) == 0);
                r["phase"] = "COMPLETED";
                save();
                return { { { "transfer_id", record.id() }, { "output", file }, { "published", true } },
                         {},
                         RemoteOutcome::ConfirmedSuccess,
                         LocalState::FilePublished };
            }
            check(errno == ENOENT && phase != "COMPLETED", "PUBLISH_CONFLICT");
        }
        struct stat present{};
        check(fstatat(parent.value, name.c_str(), &present, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT,
              "OUTPUT_EXISTS");
        if (!existing) {
            fd = Fd(openat(parent.value, part.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
            safeFile(fd.value);
            check(fsync(parent.value) == 0);
            r["part_identity"] = identity(fd.value);
            r["phase"] = "RECEIVING";
            save();
        } else {
            fd = Fd(openat(parent.value, part.c_str(), O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
            safeFile(fd.value);
            check(sameInode(identity(fd.value), r.at("part_identity")), "PART_REPLACED");
        }
        auto offset = quantity(r, "durable_offset");
        check(offset <= length && status(fd.value).st_size >= offset, "PART_TRUNCATED");
        if (status(fd.value).st_size > offset) {
            check(ftruncate(fd.value, offset) == 0 && fdatasync(fd.value) == 0);
        }
        check(lseek(fd.value, offset, SEEK_SET) == offset);
        local = LocalState::DownloadPartial;
        auto durable = offset;
        auto sync = [&] {
            check(fdatasync(fd.value) == 0);
            r["durable_offset"] = std::to_string(offset);
            save();
            durable = offset;
        };
        if (offset < length) {
            TransferRequest req{ Method::Get, url, bearer, {}, 0, {}, {}, {} };
            if (existing) {
                req.headers = { { "Range", "bytes=" + std::to_string(offset) + "-" }, { "If-Range", etag } };
            } else
                req.headers = { { "If-Match", etag } };
            const auto begin = offset;
            req.validate = [&](const HttpResponse& response) -> ClientResult<Done> {
                try {
                    if (response.status >= 400) return Done{};
                    if (response.status != 200 && response.status != 206) fail(ErrorKind::Protocol);
                    if (field(response, "etag") != etag ||
                        field(response, "content-type") != "application/octet-stream" ||
                        response.headers.count("transfer-encoding") ||
                        (response.headers.count("content-encoding") &&
                         field(response, "content-encoding") != "identity") ||
                        decimal(field(response, "content-length")) != length - begin)
                        fail(ErrorKind::Protocol, "REPRESENTATION_CHANGED");
                    if (response.status == 200 && begin != 0) fail(ErrorKind::Protocol, "RANGE_IGNORED");
                    if (response.status == 206 && field(response, "content-range") !=
                                                      "bytes " + std::to_string(begin) + "-" +
                                                          std::to_string(length - 1) + "/" + std::to_string(length))
                        fail(ErrorKind::Protocol, "INVALID_CONTENT_RANGE");
                    return Done{};
                } catch (const Failure& e) {
                    return e.error;
                } catch (...) {
                    return failure(ErrorKind::Protocol);
                }
            };
            req.sink = [&](std::string_view bytes) -> ClientResult<Done> {
                try {
                    check(static_cast<std::int64_t>(bytes.size()) <= length - offset);
                    writeAll(fd.value, bytes);
                    offset += static_cast<std::int64_t>(bytes.size());
                    if (offset - durable >= kMaxPatchBytes) sync();
                    return Done{};
                } catch (const Failure& e) {
                    return e.error;
                }
            };
            auto response = binary_.transfer(p.server, req, cancel_);
            sync();
            if (!response) throw Failure{ response.error() };
            if (response.value().status >= 400) {
                auto rejected = decodeTransfer(TransferOperation::Show, response.value());
                if (!rejected) throw Failure{ rejected.error() };
                fail(ErrorKind::Protocol);
            }
            if (offset != length) fail(ErrorKind::Protocol, "INCOMPLETE_DOWNLOAD");
        }
        check(status(fd.value).st_size == length);
        if (hash(fd.value, cancel_) != digest) {
            r["phase"] = "FAILED";
            save();
            fail(ErrorKind::Protocol, "CHECKSUM_MISMATCH");
        }
        check(fdatasync(fd.value) == 0);
        r["phase"] = "PUBLISHING";
        save();
        local = LocalState::Unknown;
        if (syscall(SYS_renameat2, parent.value, part.c_str(), parent.value, name.c_str(), RENAME_NOREPLACE) != 0)
            fail(ErrorKind::LocalState, "PUBLISH_CONFLICT");
        local = LocalState::Unknown;
        check(fsync(parent.value) == 0);
        r["phase"] = "COMPLETED";
        save();
        local = LocalState::FilePublished;
        return { { { "transfer_id", record.id() }, { "output", file }, { "published", true } },
                 {},
                 RemoteOutcome::ConfirmedSuccess,
                 local };
    } catch (Failure& e) {
        if (e.error.remote == RemoteOutcome::NotSent) e.error.remote = remote;
        if (e.error.local == LocalState::Unchanged) e.error.local = local;
        e.error.transferId = record.id();
        throw;
    } catch (...) {
        auto e = failure(ErrorKind::LocalState);
        e.remote = remote;
        e.local = local;
        e.transferId = record.id();
        throw Failure{ e };
    }
}
}  // namespace anof::client
