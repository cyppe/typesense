#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>

#include <curl/curl.h>

#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "http_client.h"
#include "nuraft/nuraft_http_runtime.h"
#include "nuraft/nuraft_snapshot_coordinator.h"
#include "nuraft/nuraft_state_initializer.h"
#include "test/runfiles_utils.h"
#include "test/temp_dir_utils.h"

namespace {

constexpr const char* kBooksCollectionSchema = R"({
  "name":"books",
  "fields":[
    {"name":"title","type":"string"}
  ]
})";

uint32_t pick_free_port() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_NE(fd, -1);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    EXPECT_EQ(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    socklen_t len = sizeof(addr);
    EXPECT_EQ(getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len), 0);

    const uint32_t port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

class NuRaftHttpRuntimeHarness {
public:
    NuRaftHttpRuntimeHarness() = default;

    ~NuRaftHttpRuntimeHarness() {
        stop();
    }

    bool start(const NuRaftHttpServerOptions& options, std::string& error) {
        stop();
        options_ = options;
        const std::string binary_path = resolve_test_path({
            "bazel-bin/typesense-server",
            "typesense-server",
        });
        log_path_ = (std::filesystem::path(options_.startup_options.data_dir) / "runtime.log").string();

        std::vector<std::string> args = {
            binary_path,
            "--data-dir", options_.startup_options.data_dir,
            "--node-host", options_.startup_options.local_host,
            "--listen-address", options_.listen_address,
            "--listen-port", std::to_string(options_.listen_port),
            "--api-port", std::to_string(options_.startup_options.api_port),
            "--peering-port", std::to_string(options_.startup_options.peer_port),
            "--api-key", options_.api_key,
        };
        if (!options_.startup_options.nodes_config.empty()) {
            args.insert(args.end(), {"--nodes", options_.startup_options.nodes_config});
        }
        if (options_.startup_options.api_uses_ssl) {
            args.push_back("--api-uses-ssl");
        }

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (std::string& argument : args) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);

        child_pid_ = fork();
        if (child_pid_ == -1) {
            error = "fork() failed";
            return false;
        }

        if (child_pid_ == 0) {
            const int log_fd = open(log_path_.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (log_fd != -1) {
                dup2(log_fd, STDOUT_FILENO);
                dup2(log_fd, STDERR_FILENO);
                close(log_fd);
            }
            execv(binary_path.c_str(), argv.data());
            _exit(127);
        }

        return wait_until_ready(error);
    }

    void stop() {
        if (child_pid_ > 0) {
            kill(child_pid_, SIGKILL);
            int status = 0;
            waitpid(child_pid_, &status, 0);
            exit_status_ = status;
            child_pid_ = -1;
        }
    }

    std::string base_url() const {
        return "http://" + options_.listen_address + ":" + std::to_string(options_.listen_port);
    }

    std::string log_path() const {
        return log_path_;
    }

    bool is_running() const {
        return child_pid_ > 0;
    }

private:
    bool wait_until_ready(std::string& error) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (int attempt = 0; attempt < 400; ++attempt) {
            std::string response;
            std::map<std::string, std::string> headers;
            const long status = HttpClient::get_response(base_url() + "/health",
                                                         response,
                                                         headers,
                                                         {},
                                                         200,
                                                         true);
            if (status == 200) {
                error.clear();
                return true;
            }

            if (child_pid_ > 0) {
                int wait_status = 0;
                const pid_t result = waitpid(child_pid_, &wait_status, WNOHANG);
                if (result == child_pid_) {
                    exit_status_ = wait_status;
                    child_pid_ = -1;
                    error = "NuRaft HTTP runtime server exited before becoming healthy. Log: " + log_path_;
                    return false;
                }
            } else {
                error = "NuRaft HTTP runtime server exited before becoming healthy. Log: " + log_path_;
                return false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        error = "Timed out waiting for NuRaft HTTP runtime health endpoint. Log: " + log_path_;
        return false;
    }

    NuRaftHttpServerOptions options_;
    std::string log_path_;
    pid_t child_pid_ = -1;
    int exit_status_ = 0;
};

class NuRaftHttpRuntimeTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        curl_global_init(CURL_GLOBAL_SSL);
        HttpClient::get_instance().init("xyz");
    }

    static void TearDownTestSuite() {
        curl_global_cleanup();
    }

    void SetUp() override {
        temp_dir_ = typesense_test::make_test_temp_dir("nuraft_http_runtime_test");
        typesense_test::reset_test_temp_dir(temp_dir_);
    }

    void TearDown() override {
        node1_.stop();
        node2_.stop();
        typesense_test::cleanup_test_temp_dir(temp_dir_);
    }

    std::string node_dir(const std::string& name) const {
        return (std::filesystem::path(temp_dir_) / name).string();
    }

    static nlohmann::json parse_json(const std::string& encoded) {
        return nlohmann::json::parse(encoded);
    }

    std::string temp_dir_;
    NuRaftHttpRuntimeHarness node1_;
    NuRaftHttpRuntimeHarness node2_;
};

TEST_F(NuRaftHttpRuntimeTest, PersistsHttpWritesAcrossRestartAndSnapshot) {
    const uint32_t api_port = pick_free_port();
    const uint32_t peer_port = pick_free_port();
    const std::string data_dir = node_dir("single-node");
    const std::string snapshot_dir = node_dir("exported-snapshot");

    NuRaftHttpServerOptions options;
    options.startup_options.data_dir = data_dir;
    options.startup_options.local_host = "127.0.0.1";
    options.startup_options.peer_port = peer_port;
    options.startup_options.api_port = api_port;
    options.listen_address = "127.0.0.1";
    options.listen_port = api_port;
    options.api_key = "xyz";

    std::string error;
    ASSERT_TRUE(node1_.start(options, error)) << error;

    std::string response;
    std::map<std::string, std::string> headers;
    ASSERT_EQ(201,
              HttpClient::post_response(node1_.base_url() + "/collections",
                                        kBooksCollectionSchema,
                                        response,
                                        headers,
                                        {},
                                        2000,
                                        true));
    EXPECT_EQ("books", parse_json(response)["name"].get<std::string>()) << "runtime log: " << node1_.log_path();

    response.clear();
    headers.clear();
    ASSERT_EQ(201,
              HttpClient::post_response(node1_.base_url() + "/collections/books/documents",
                                        R"({"id":"1","title":"Dune"})",
                                        response,
                                        headers,
                                        {},
                                        2000,
                                        true));
    EXPECT_EQ("1", parse_json(response)["id"].get<std::string>()) << "runtime log: " << node1_.log_path();
    EXPECT_EQ("Dune", parse_json(response)["title"].get<std::string>()) << "runtime log: " << node1_.log_path();

    response.clear();
    headers.clear();
    ASSERT_EQ(200,
              HttpClient::get_response(node1_.base_url() + "/collections/books/documents/1",
                                       response,
                                       headers,
                                       {},
                                       2000,
                                       true));
    EXPECT_EQ("Dune", parse_json(response)["title"].get<std::string>()) << "runtime log: " << node1_.log_path();

    response.clear();
    headers.clear();
    ASSERT_EQ(201,
              HttpClient::post_response(node1_.base_url() + "/operations/snapshot?snapshot_path=" + snapshot_dir,
                                        "",
                                        response,
                                        headers,
                                        {},
                                        2000,
                                        true));
    EXPECT_TRUE(parse_json(response)["success"].get<bool>()) << "runtime log: " << node1_.log_path();

    node1_.stop();

    ASSERT_TRUE(node1_.start(options, error)) << error;

    response.clear();
    headers.clear();
    ASSERT_EQ(200,
              HttpClient::get_response(node1_.base_url() + "/collections/books",
                                       response,
                                       headers,
                                       {},
                                       2000,
                                       true));
    EXPECT_EQ("books", parse_json(response)["name"].get<std::string>()) << "runtime log: " << node1_.log_path();

    response.clear();
    headers.clear();
    ASSERT_EQ(200,
              HttpClient::get_response(node1_.base_url() + "/collections/books/documents/1",
                                       response,
                                       headers,
                                       {},
                                       2000,
                                       true));
    EXPECT_EQ("1", parse_json(response)["id"].get<std::string>()) << "runtime log: " << node1_.log_path();
    EXPECT_EQ("Dune", parse_json(response)["title"].get<std::string>()) << "runtime log: " << node1_.log_path();
}

TEST_F(NuRaftHttpRuntimeTest, InstallsSnapshotIntoFreshHttpRuntimeNode) {
    const uint32_t api_port_1 = pick_free_port();
    const uint32_t peer_port_1 = pick_free_port();
    const uint32_t api_port_2 = pick_free_port();
    const uint32_t peer_port_2 = pick_free_port();
    const std::string source_dir = node_dir("snapshot-source");
    const std::string target_dir = node_dir("snapshot-target");
    const std::string snapshot_dir = node_dir("snapshot-export");

    NuRaftHttpServerOptions source_options;
    source_options.startup_options.data_dir = source_dir;
    source_options.startup_options.local_host = "127.0.0.1";
    source_options.startup_options.peer_port = peer_port_1;
    source_options.startup_options.api_port = api_port_1;
    source_options.listen_address = "127.0.0.1";
    source_options.listen_port = api_port_1;
    source_options.api_key = "xyz";

    std::string error;
    ASSERT_TRUE(node1_.start(source_options, error)) << error;

    std::string response;
    std::map<std::string, std::string> headers;
    ASSERT_EQ(201,
              HttpClient::post_response(node1_.base_url() + "/collections",
                                        kBooksCollectionSchema,
                                        response,
                                        headers,
                                        {},
                                        2000,
                                        true));
    EXPECT_EQ("books", parse_json(response)["name"].get<std::string>()) << "runtime log: " << node1_.log_path();

    response.clear();
    headers.clear();
    ASSERT_EQ(201,
              HttpClient::post_response(node1_.base_url() + "/collections/books/documents",
                                        R"({"id":"9","title":"Hyperion"})",
                                        response,
                                        headers,
                                        {},
                                        2000,
                                        true));
    EXPECT_EQ("9", parse_json(response)["id"].get<std::string>()) << "runtime log: " << node1_.log_path();

    response.clear();
    headers.clear();
    ASSERT_EQ(201,
              HttpClient::post_response(node1_.base_url() + "/operations/snapshot?snapshot_path=" + snapshot_dir,
                                        "",
                                        response,
                                        headers,
                                        {},
                                        2000,
                                        true));
    EXPECT_TRUE(parse_json(response)["success"].get<bool>()) << "runtime log: " << node1_.log_path();

    node1_.stop();

    // Install the exported snapshot into target_dir by initializing the target node
    // and then using NuRaftSnapshotCoordinator to install the snapshot.
    {
        NuRaftPrototypeOptions init_options;
        init_options.data_dir = target_dir;
        init_options.local_host = "127.0.0.1";
        init_options.peer_port = peer_port_2;
        init_options.api_port = api_port_2;
        NuRaftIdentity target_identity;
        NuRaftBootstrapConfig target_bootstrap;
        ASSERT_TRUE(NuRaftStateInitializer::initialize(init_options, target_identity,
                                                        target_bootstrap, error)) << error;

        NuRaftSnapshotCoordinator coordinator(NuRaftStateLayout::from_data_dir(target_dir));
        NuRaftSnapshotDescriptor installed_descriptor;
        ASSERT_TRUE(coordinator.install_snapshot(snapshot_dir, installed_descriptor, error)) << error;
    }

    NuRaftHttpServerOptions target_options;
    target_options.startup_options.data_dir = target_dir;
    target_options.startup_options.local_host = "127.0.0.1";
    target_options.startup_options.peer_port = peer_port_2;
    target_options.startup_options.api_port = api_port_2;
    target_options.listen_address = "127.0.0.1";
    target_options.listen_port = api_port_2;
    target_options.api_key = "xyz";
    ASSERT_TRUE(node2_.start(target_options, error)) << error;

    response.clear();
    headers.clear();
    ASSERT_EQ(200,
              HttpClient::get_response(node2_.base_url() + "/collections/books/documents/9",
                                       response,
                                       headers,
                                       {},
                                       2000,
                                       true));
    EXPECT_EQ("Hyperion", parse_json(response)["title"].get<std::string>()) << "runtime log: " << node2_.log_path();

    response.clear();
    headers.clear();
    ASSERT_EQ(200,
              HttpClient::get_response(node2_.base_url() + "/status",
                                       response,
                                       headers,
                                       {},
                                       2000,
                                       true));
    const nlohmann::json status = parse_json(response);
    EXPECT_TRUE(status["is_leader"].get<bool>());
    EXPECT_EQ("http://127.0.0.1:" + std::to_string(api_port_2) + "/", status["leader_url"].get<std::string>());
}

}  // namespace
