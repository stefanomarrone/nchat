// ntfy.h
//
// Copyright (c) 2026 Stefano Marrone
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct NtfyMessage
{
  std::string topic;
  std::string title;
  std::string message;
  int priority = 3;
  std::vector<std::string> tags;
  std::string click;
};

// Asynchronous ntfy publisher. Messages are queued and published by a worker
// thread using the curl command line tool, so protocol threads never block on
// network i/o. All request data (including the access token) is passed to curl
// on stdin, never on its command line, so it is not visible in the process list.
class Ntfy
{
public:
  Ntfy();
  virtual ~Ntfy();

  void Init(const std::string& p_Server, const std::string& p_Token, int p_TimeoutSec);
  void Cleanup();
  void Publish(const NtfyMessage& p_Message);

  static bool IsCurlAvailable();

private:
  void Process();
  bool Send(const NtfyMessage& p_Message);
  std::string GetCurlConfig(const NtfyMessage& p_Message) const;

  static std::string JsonEscape(const std::string& p_Str);
  static std::string CurlConfigEscape(const std::string& p_Str);
  static bool RunCurl(const std::vector<std::string>& p_Args, const std::string& p_Stdin);

private:
  std::string m_Server;
  std::string m_Token;
  int m_TimeoutSec = 15;

  std::thread m_Thread;
  std::mutex m_Mutex;
  std::condition_variable m_CondVar;
  std::deque<NtfyMessage> m_Queue;
  bool m_Running = false;
};
