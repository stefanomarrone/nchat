// ui.h
//
// Copyright (c) 2026 Stefano Marrone
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#pragma once

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

#include "config.h"
#include "ntfy.h"
#include "protocol.h"

class Protocol;
class ServiceMessage;

// Headless "ui" for the nchat-notify daemon. It implements the interface that
// src/main.cpp expects, but instead of presenting chats it watches a configured
// set of chats and publishes a preconfigured ntfy notification when a new
// incoming message arrives in one of them.
class Ui
{
public:
  Ui();
  virtual ~Ui();

  void Init();
  void Cleanup();

  void Run();
  void AddProtocol(std::shared_ptr<Protocol> p_Protocol);
  std::unordered_map<std::string, std::shared_ptr<Protocol>>& GetProtocols();
  void MessageHandler(std::shared_ptr<ServiceMessage> p_ServiceMessage);

  static void RunKeyDump();
  static void ShowHelp();

private:
  void InitConfig();
  void LoadState();
  void SaveState();
  void SaveChatList();

  void HandleConnectNotify(std::shared_ptr<ConnectNotify> p_ConnectNotify);
  void HandleNewMessagesNotify(std::shared_ptr<NewMessagesNotify> p_NewMessagesNotify);
  void HandleRequestAppExitNotify(std::shared_ptr<RequestAppExitNotify> p_RequestAppExitNotify);

  bool IsWatched(const std::string& p_ProfileId, const std::string& p_ChatId) const;
  int64_t GetBaseline(const std::string& p_Key);
  std::string GetName(const std::string& p_ProfileId, const std::string& p_Id) const;
  std::string Expand(const std::string& p_Template, const std::string& p_ProfileId, const std::string& p_ChatId,
                     const ChatMessage& p_ChatMessage) const;
  void NotifyMessage(const std::string& p_ProfileId, const std::string& p_ChatId, const ChatMessage& p_ChatMessage);
  void NotifyStatus(const std::string& p_Text);

  static std::string GetKey(const std::string& p_ProfileId, const std::string& p_ChatId);

private:
  std::mutex m_Mutex;
  std::condition_variable m_CondVar;
  bool m_ExitRequested = false;

  std::unordered_map<std::string, std::shared_ptr<Protocol>> m_Protocols;
  std::unique_ptr<Config> m_Config;
  Ntfy m_Ntfy;

  // settings
  std::string m_Topic;
  std::string m_StatusTopic;
  std::string m_Title;
  std::string m_Message;
  int m_Priority = 3;
  std::vector<std::string> m_Tags;
  std::string m_Click;
  int64_t m_CooldownMs = 0;
  int64_t m_CatchupMaxAgeMs = 0;
  bool m_StatusNotify = true;
  bool m_WatchAll = false;
  std::set<std::string> m_WatchKeys;    // "profileid:chatid"
  std::set<std::string> m_WatchChatIds; // "chatid", any profile

  // runtime state
  int64_t m_StartTime = 0;
  std::string m_StatePath;
  std::string m_ChatListPath;
  std::map<std::string, int64_t> m_Watermarks; // persisted: newest notified message time per chat
  std::map<std::string, int64_t> m_Baselines;  // per session: only notify messages newer than this
  std::map<std::string, int64_t> m_LastNotifyTime;
  std::map<std::string, std::set<std::string>> m_NotifiedMsgIds;
  std::map<std::string, std::set<std::string>> m_ChatIds;                    // profile -> chat ids
  std::map<std::string, std::map<std::string, std::string>> m_ContactNames; // profile -> id -> name
  bool m_StateDirty = false;
  bool m_ChatListDirty = false;
};
