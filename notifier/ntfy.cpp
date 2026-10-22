// ntfy.cpp
//
// Copyright (c) 2026 Stefano Marrone
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#include "ntfy.h"

#include <cerrno>
#include <chrono>
#include <cstdio>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "log.h"

extern char** environ;

static const int s_MaxAttempts = 3;
static const int s_RetryDelaySec[s_MaxAttempts] = { 0, 5, 30 };
static const int s_CleanupFlushSec = 10;

Ntfy::Ntfy()
{
}

Ntfy::~Ntfy()
{
  Cleanup();
}

void Ntfy::Init(const std::string& p_Server, const std::string& p_Token, int p_TimeoutSec)
{
  m_Server = p_Server;
  while (!m_Server.empty() && (m_Server.back() == '/'))
  {
    m_Server.pop_back();
  }

  m_Token = p_Token;
  m_TimeoutSec = (p_TimeoutSec > 0) ? p_TimeoutSec : 15;

  std::unique_lock<std::mutex> lock(m_Mutex);
  if (!m_Running)
  {
    m_Running = true;
    m_Thread = std::thread(&Ntfy::Process, this);
  }
}

void Ntfy::Cleanup()
{
  {
    std::unique_lock<std::mutex> lock(m_Mutex);
    if (!m_Running) return;

    // give queued messages (e.g. a final status message) a chance to be sent
    m_CondVar.wait_for(lock, std::chrono::seconds(s_CleanupFlushSec), [&]() { return m_Queue.empty(); });
    m_Running = false;
    m_CondVar.notify_all();
  }

  if (m_Thread.joinable())
  {
    m_Thread.join();
  }

  if (!m_Queue.empty())
  {
    LOG_WARNING("ntfy dropping %d unsent message(s)", (int)m_Queue.size());
    m_Queue.clear();
  }
}

void Ntfy::Publish(const NtfyMessage& p_Message)
{
  std::unique_lock<std::mutex> lock(m_Mutex);
  if (!m_Running)
  {
    LOG_WARNING("ntfy not running, dropping message");
    return;
  }

  m_Queue.push_back(p_Message);
  m_CondVar.notify_all();
}

bool Ntfy::IsCurlAvailable()
{
  return RunCurl(std::vector<std::string>({ "curl", "--version" }), "");
}

void Ntfy::Process()
{
  // a curl process exiting early must yield EPIPE on write, not terminate the daemon
  sigset_t sigset;
  sigemptyset(&sigset);
  sigaddset(&sigset, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &sigset, nullptr);

  std::unique_lock<std::mutex> lock(m_Mutex);
  while (true)
  {
    m_CondVar.wait(lock, [&]() { return !m_Queue.empty() || !m_Running; });
    if (m_Queue.empty() && !m_Running) break;

    if (m_Queue.empty()) continue;

    const NtfyMessage message = m_Queue.front();

    bool sent = false;
    for (int attempt = 0; (attempt < s_MaxAttempts) && !sent; ++attempt)
    {
      if (s_RetryDelaySec[attempt] > 0)
      {
        // abort retry delay on shutdown, but still make the attempt
        m_CondVar.wait_for(lock, std::chrono::seconds(s_RetryDelaySec[attempt]), [&]() { return !m_Running; });
      }

      lock.unlock();
      sent = Send(message);
      lock.lock();

      if (!sent)
      {
        LOG_WARNING("ntfy publish to \"%s\" failed (attempt %d/%d)", message.topic.c_str(), attempt + 1,
                    s_MaxAttempts);
      }

      if (!m_Running) break;
    }

    if (sent)
    {
      LOG_DEBUG("ntfy published to \"%s\"", message.topic.c_str());
    }
    else
    {
      LOG_ERROR("ntfy publish to \"%s\" failed, message dropped", message.topic.c_str());
    }

    m_Queue.pop_front();
    m_CondVar.notify_all();
  }
}

bool Ntfy::Send(const NtfyMessage& p_Message)
{
  // config is read from stdin ("-K -"), see GetCurlConfig()
  const std::vector<std::string> args =
  {
    "curl", "--silent", "--show-error", "--fail", "--output", "/dev/null",
    "--max-time", std::to_string(m_TimeoutSec), "--config", "-",
  };

  return RunCurl(args, GetCurlConfig(p_Message));
}

std::string Ntfy::GetCurlConfig(const NtfyMessage& p_Message) const
{
  // publish as json to the server root, which (unlike http headers) supports utf-8 titles
  std::string json = "{\"topic\":\"" + JsonEscape(p_Message.topic) + "\"";
  if (!p_Message.title.empty())
  {
    json += ",\"title\":\"" + JsonEscape(p_Message.title) + "\"";
  }

  json += ",\"message\":\"" + JsonEscape(p_Message.message) + "\"";
  json += ",\"priority\":" + std::to_string(p_Message.priority);
  if (!p_Message.tags.empty())
  {
    json += ",\"tags\":[";
    for (size_t i = 0; i < p_Message.tags.size(); ++i)
    {
      json += ((i > 0) ? ",\"" : "\"") + JsonEscape(p_Message.tags.at(i)) + "\"";
    }

    json += "]";
  }

  if (!p_Message.click.empty())
  {
    json += ",\"click\":\"" + JsonEscape(p_Message.click) + "\"";
  }

  json += "}";

  std::string config;
  config += "url = \"" + CurlConfigEscape(m_Server + "/") + "\"\n";
  config += "header = \"Content-Type: application/json\"\n";
  if (!m_Token.empty())
  {
    config += "header = \"" + CurlConfigEscape("Authorization: Bearer " + m_Token) + "\"\n";
  }

  config += "data-raw = \"" + CurlConfigEscape(json) + "\"\n";
  return config;
}

std::string Ntfy::JsonEscape(const std::string& p_Str)
{
  std::string rv;
  rv.reserve(p_Str.size());
  for (unsigned char ch : p_Str)
  {
    switch (ch)
    {
      case '"': rv += "\\\""; break;
      case '\\': rv += "\\\\"; break;
      case '\b': rv += "\\b"; break;
      case '\f': rv += "\\f"; break;
      case '\n': rv += "\\n"; break;
      case '\r': rv += "\\r"; break;
      case '\t': rv += "\\t"; break;
      default:
        if (ch < 0x20)
        {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", ch);
          rv += buf;
        }
        else
        {
          rv += (char)ch;
        }
        break;
    }
  }

  return rv;
}

std::string Ntfy::CurlConfigEscape(const std::string& p_Str)
{
  // json escaping above leaves no raw control chars, only quotes and backslashes need escaping
  std::string rv;
  rv.reserve(p_Str.size());
  for (char ch : p_Str)
  {
    if ((ch == '"') || (ch == '\\'))
    {
      rv += '\\';
    }

    rv += ch;
  }

  return rv;
}

bool Ntfy::RunCurl(const std::vector<std::string>& p_Args, const std::string& p_Stdin)
{
  int pipefd[2] = { -1, -1 };
  if (pipe(pipefd) != 0)
  {
    LOG_ERROR("pipe failed errno %d", errno);
    return false;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipefd[0], STDIN_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipefd[0]);
  posix_spawn_file_actions_addclose(&actions, pipefd[1]);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);

  // child must not inherit the caller's blocked SIGPIPE
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  sigset_t emptyset;
  sigemptyset(&emptyset);
  posix_spawnattr_setsigmask(&attr, &emptyset);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);

  std::vector<char*> argv;
  for (const std::string& arg : p_Args)
  {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }

  argv.push_back(nullptr);

  pid_t pid = -1;
  const int spawnrv = posix_spawnp(&pid, argv.at(0), &actions, &attr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  close(pipefd[0]);

  if (spawnrv != 0)
  {
    LOG_ERROR("spawn %s failed errno %d", argv.at(0), spawnrv);
    close(pipefd[1]);
    return false;
  }

  const char* data = p_Stdin.data();
  size_t remaining = p_Stdin.size();
  while (remaining > 0)
  {
    const ssize_t written = write(pipefd[1], data, remaining);
    if (written < 0)
    {
      if (errno == EINTR) continue;

      LOG_WARNING("write to curl failed errno %d", errno);
      break;
    }

    data += written;
    remaining -= written;
  }

  close(pipefd[1]);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0)
  {
    if (errno != EINTR)
    {
      LOG_ERROR("waitpid failed errno %d", errno);
      return false;
    }
  }

  if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0))
  {
    LOG_WARNING("curl exited with status %d", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return false;
  }

  return true;
}
