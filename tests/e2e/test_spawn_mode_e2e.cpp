// Copyright (C) 2026 Christian Mayer and the CometVisu contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

/**
 * @file test_spawn_mode_e2e.cpp
 * @brief End-to-end test of the spawn-fcgi mode of the real binary.
 *
 * Without `FCGI_SOCKET` the backend serves FastCGI on stdin/stdout, the way
 * spawn-fcgi and the reference eibread-cgi / eibwrite-cgi backends used it.
 * Only the deployed executable can exercise that path: libfcgi keeps the FCGI
 * stdio layer in process-global state that cannot be initialised next to the
 * FCGX API used by the direct-socket mode, so this mode is driven through the
 * real process instead of in-process.
 */

#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

/// One FastCGI record (version 1, no padding).
Bytes Record(std::uint8_t type, const Bytes& content) {
  Bytes out{0x01,
            type,
            0x00,
            0x01,  // request id 1
            static_cast<std::uint8_t>(content.size() >> 8),
            static_cast<std::uint8_t>(content.size() & 0xFF),
            0x00,
            0x00};
  out.insert(out.end(), content.begin(), content.end());
  return out;
}

Bytes Param(const std::string& name, const std::string& value) {
  Bytes out{static_cast<std::uint8_t>(name.size()), static_cast<std::uint8_t>(value.size())};
  out.insert(out.end(), name.begin(), name.end());
  out.insert(out.end(), value.begin(), value.end());
  return out;
}

/// A complete FastCGI request for the given endpoint.
Bytes Request(const std::string& endpoint) {
  Bytes request = Record(0x01, Bytes{0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  Bytes params = Param("REQUEST_METHOD", "GET");
  const Bytes uri = Param("REQUEST_URI", "/cgi-bin" + endpoint);
  const Bytes script = Param("SCRIPT_NAME", "/cgi-bin" + endpoint);
  const Bytes gateway = Param("GATEWAY_INTERFACE", "CGI/1.1");
  for (const Bytes* block : {&uri, &script, &gateway}) {
    params.insert(params.end(), block->begin(), block->end());
  }
  const Bytes params_record = Record(0x04, params);
  request.insert(request.end(), params_record.begin(), params_record.end());
  const Bytes params_end = Record(0x04, {});
  request.insert(request.end(), params_end.begin(), params_end.end());
  const Bytes stdin_end = Record(0x05, {});
  request.insert(request.end(), stdin_end.begin(), stdin_end.end());
  return request;
}

/// Extra environment entries for the child ("KEY=VALUE").
using ExtraEnv = std::vector<std::string>;

/// Start the backend in spawn-fcgi mode, feed it one request and collect its
/// stdout.  Returns the child's exit status and fills @p output.
int RunSpawnMode(const Bytes& request, std::string* output, const ExtraEnv& env = {}) {
  int in_pipe[2] = {-1, -1};
  int out_pipe[2] = {-1, -1};
  if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0) {
    return -1;
  }

  const pid_t pid = ::fork();
  if (pid == 0) {
    ::dup2(in_pipe[0], STDIN_FILENO);
    ::dup2(out_pipe[1], STDOUT_FILENO);
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
    ::close(in_pipe[0]);
    ::close(in_pipe[1]);
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    // An unprivileged transport that needs no hardware: the FastCGI layer is
    // what this test covers, not the bus.
    ::setenv("KNX_TRANSPORT", "ip-routing", 1);
    ::unsetenv("FCGI_SOCKET");
    ::unsetenv("DEBUG_BACKEND");
    for (const std::string& entry : env) {
      const auto equals = entry.find('=');
      if (equals != std::string::npos) {
        ::setenv(entry.substr(0, equals).c_str(), entry.substr(equals + 1).c_str(), 1);
      }
    }
    ::execl(CVKNX_BINARY, CVKNX_BINARY, static_cast<char*>(nullptr));
    ::_exit(127);
  }

  ::close(in_pipe[0]);
  ::close(out_pipe[1]);
  size_t sent = 0;
  while (sent < request.size()) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const ssize_t written = ::write(in_pipe[1], request.data() + sent, request.size() - sent);
    if (written <= 0) {
      break;
    }
    sent += static_cast<size_t>(written);
  }
  ::close(in_pipe[1]);  // EOF ends the accept loop

  char buffer[512] = {};
  ssize_t n = 0;
  while ((n = ::read(out_pipe[0], buffer, sizeof(buffer))) > 0) {
    output->append(buffer, static_cast<size_t>(n));
  }
  ::close(out_pipe[0]);

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

TEST(SpawnModeE2ETest, AnswersARequestOnStdout) {
  std::string output;
  const int code = RunSpawnMode(Request("/l"), &output);
  ASSERT_EQ(code, 0) << output;
  // The response is written to stdout as an HTTP-style status line plus body.
  EXPECT_NE(output.find("Status: "), std::string::npos) << output;
  EXPECT_NE(output.find("Content-Type: application/json"), std::string::npos) << output;
  EXPECT_NE(output.find("Content-Length: "), std::string::npos) << output;
}

TEST(SpawnModeE2ETest, UnknownEndpointIsReported) {
  std::string output;
  const int code = RunSpawnMode(Request("/nope"), &output);
  ASSERT_EQ(code, 0) << output;
  EXPECT_NE(output.find("Status: 404"), std::string::npos) << output;
}

// ================================================================
// POST body handling in spawn-fcgi mode
// ================================================================
//
// In this mode read_request() takes the request parameters from the process
// environment (libfcgi exports them there).  The body comes from the FastCGI
// stdin stream.  The test sets the variables itself so the body code paths are
// reachable without depending on which parameters a libfcgi build exports.

TEST(SpawnModeE2ETest, PostBodyIsReadUpToContentLength) {
  std::string output;
  const int code =
      RunSpawnMode(Request("/w"), &output, {"REQUEST_METHOD=POST", "CONTENT_LENGTH=8"});
  ASSERT_EQ(code, 0) << output;
  // The request is answered; the body itself comes from the FCGI stdin stream,
  // which already reached EOF after the parameter block.
  EXPECT_NE(output.find("Status: "), std::string::npos) << output;
}

TEST(SpawnModeE2ETest, InvalidContentLengthIsIgnored) {
  std::string output;
  const int code =
      RunSpawnMode(Request("/w"), &output, {"REQUEST_METHOD=PUT", "CONTENT_LENGTH=abc"});
  ASSERT_EQ(code, 0) << output;
  EXPECT_NE(output.find("Status: "), std::string::npos) << output;
}

TEST(SpawnModeE2ETest, EmptyContentLengthSkipsTheBody) {
  std::string output;
  const int code = RunSpawnMode(Request("/w"), &output, {"REQUEST_METHOD=POST", "CONTENT_LENGTH="});
  ASSERT_EQ(code, 0) << output;
  EXPECT_NE(output.find("Status: "), std::string::npos) << output;
}

TEST(SpawnModeE2ETest, ZeroContentLengthSkipsTheBody) {
  std::string output;
  const int code =
      RunSpawnMode(Request("/w"), &output, {"REQUEST_METHOD=POST", "CONTENT_LENGTH=0"});
  ASSERT_EQ(code, 0) << output;
  EXPECT_NE(output.find("Status: "), std::string::npos) << output;
}

TEST(SpawnModeE2ETest, ExcessiveContentLengthIsCapped) {
  std::string output;
  const int code =
      RunSpawnMode(Request("/w"), &output, {"REQUEST_METHOD=POST", "CONTENT_LENGTH=100000000"});
  ASSERT_EQ(code, 0) << output;
  EXPECT_NE(output.find("Status: "), std::string::npos) << output;
}

}  // namespace
