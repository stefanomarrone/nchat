// ui.cpp
//
// Copyright (c) 2026 Stefano Marrone
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#include "ui.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>

#include "apputil.h"
#include "fileutil.h"
#include "log.h"
#include "strutil.h"
#include "timeutil.h"

static const size_t s_MaxTextLen = 1024;
static const size_t s_MaxNotifiedMsgIds = 5000;

// log to file and to stderr (journald when run as a systemd service)
static void Notice(const std::string& p_Str)
{
  LOG_INFO("%s", p_Str.c_str());
  std::cerr << "nchat-notify: " << p_Str << std::endl;
}

static void Alert(const std::string& p_Str)
{
  LOG_ERROR("%s", p_Str.c_str());
  std::cerr << "nchat-notify: error: " << p_Str << std::endl;
}

static std::string TruncateUtf8(const std::string& p_Str, size_t p_MaxLen)
{
  if (p_Str.size() <= p_MaxLen) return p_Str;

  size_t len = p_MaxLen;
  while ((len > 0) && ((static_cast<unsigned char>(p_Str[len]) & 0xC0) == 0x80))
  {
    --len;
  }

  return p_Str.substr(0, len) + "...";
}

static std::vector<std::string> SplitList(const std::string& p_Str)
{
  std::vector<std::string> rv;
  for (std::string item : StrUtil::Split(p_Str, ','))
  {
    StrUtil::Trim(item);
    if (!item.empty())
    {
      rv.push_back(item);
    }
  }

  return rv;
}

Ui::Ui()
{
  m_StartTime = TimeUtil::GetCurrentTimeMSec();
  m_StatePath = FileUtil::GetApplicationDir() + "/notifier_state.txt";
  m_ChatListPath = FileUtil::GetApplicationDir() + "/notifier_chats.txt";
  InitConfig();
  LoadState();
}

Ui::~Ui()
{
}

void Ui::Init()
{
  if (m_Topic.empty())
  {
    Alert("ntfy_topic not set in " + FileUtil::GetApplicationDir() + "/notifier.conf");
    m_ExitRequested = true;
    return;
  }

  if (!Ntfy::IsCurlAvailable())
  {
    Alert("curl not found, it is required to publish ntfy notifications");
    m_ExitRequested = true;
    return;
  }

  if (!m_WatchAll && m_WatchKeys.empty() && m_WatchChatIds.empty())
  {
    Notice("watch_chats is empty, no messages will be notified (see " + m_ChatListPath + " for chat ids)");
  }

  m_Ntfy.Init(m_Config->Get("ntfy_server"), m_Config->Get("ntfy_token"),
              (int)StrUtil::ToInteger(m_Config->Get("ntfy_timeout_sec")));
}

void Ui::Cleanup()
{
  {
    std::unique_lock<std::mutex> lock(m_Mutex);
    SaveState();
  }

  m_Ntfy.Cleanup();
}

void Ui::Run()
{
  if (m_ExitRequested) return;

  Notice("started, " + std::to_string(m_Protocols.size()) + " profile(s), " +
         (m_WatchAll ? std::string("watching all chats")
                     : ("watching " + std::to_string(m_WatchKeys.size() + m_WatchChatIds.size()) + " chat(s)")));

  std::unique_lock<std::mutex> lock(m_Mutex);
  while (!m_ExitRequested && !AppUtil::IsTerminateRequested())
  {
    m_CondVar.wait_for(lock, std::chrono::seconds(1));

    if (m_StateDirty)
    {
      SaveState();
    }

    if (m_ChatListDirty)
    {
      SaveChatList();
    }
  }

  Notice("stopping");
}

void Ui::AddProtocol(std::shared_ptr<Protocol> p_Protocol)
{
  m_Protocols[p_Protocol->GetProfileId()] = p_Protocol;
}

std::unordered_map<std::string, std::shared_ptr<Protocol>>& Ui::GetProtocols()
{
  return m_Protocols;
}

void Ui::MessageHandler(std::shared_ptr<ServiceMessage> p_ServiceMessage)
{
  std::unique_lock<std::mutex> lock(m_Mutex);
  const std::string& profileId = p_ServiceMessage->profileId;

  switch (p_ServiceMessage->GetMessageType())
  {
    case ConnectNotifyType:
      HandleConnectNotify(std::static_pointer_cast<ConnectNotify>(p_ServiceMessage));
      break;

    case NewMessagesNotifyType:
      HandleNewMessagesNotify(std::static_pointer_cast<NewMessagesNotify>(p_ServiceMessage));
      break;

    case RequestAppExitNotifyType:
      HandleRequestAppExitNotify(std::static_pointer_cast<RequestAppExitNotify>(p_ServiceMessage));
      break;

    case NewChatsNotifyType:
      {
        std::shared_ptr<NewChatsNotify> newChatsNotify = std::static_pointer_cast<NewChatsNotify>(p_ServiceMessage);
        if (newChatsNotify->success)
        {
          for (const ChatInfo& chatInfo : newChatsNotify->chatInfos)
          {
            m_ChatListDirty |= m_ChatIds[profileId].insert(chatInfo.id).second;
          }
        }
      }
      break;

    case NewContactsNotifyType:
      {
        std::shared_ptr<NewContactsNotify> newContactsNotify =
          std::static_pointer_cast<NewContactsNotify>(p_ServiceMessage);
        std::map<std::string, std::string>& names = m_ContactNames[profileId];
        if (newContactsNotify->fullSync)
        {
          names.clear();
        }

        for (const ContactInfo& contactInfo : newContactsNotify->contactInfos)
        {
          names[contactInfo.id] = contactInfo.name;
        }

        m_ChatListDirty = true;
      }
      break;

    default:
      break;
  }
}

void Ui::RunKeyDump()
{
  std::cout << "keydump not supported by nchat-notify\n";
}

void Ui::InitConfig()
{
  const std::map<std::string, std::string> defaultConfig =
  {
    { "ntfy_server", "https://ntfy.sh" },
    { "ntfy_topic", "" },
    { "ntfy_token", "" },
    { "ntfy_timeout_sec", "15" },
    { "notify_title", "%chat%" },
    { "notify_message", "New message" },
    { "notify_priority", "3" },
    { "notify_tags", "speech_balloon" },
    { "notify_click", "" },
    { "notify_cooldown_sec", "60" },
    { "watch_chats", "" },
    { "catchup_max_age_sec", "86400" },
    { "status_notify", "1" },
    { "status_topic", "" },
  };

  const std::string configPath = FileUtil::GetApplicationDir() + "/notifier.conf";
  m_Config.reset(new Config(configPath, defaultConfig));

  m_Topic = m_Config->Get("ntfy_topic");
  m_StatusTopic = m_Config->Get("status_topic");
  if (m_StatusTopic.empty())
  {
    m_StatusTopic = m_Topic;
  }

  m_Title = m_Config->Get("notify_title");
  m_Message = m_Config->Get("notify_message");
  m_Priority = std::max(1, std::min(5, (int)StrUtil::ToInteger(m_Config->Get("notify_priority"))));
  m_Tags = SplitList(m_Config->Get("notify_tags"));
  m_Click = m_Config->Get("notify_click");
  m_CooldownMs = std::max<int64_t>(0, StrUtil::ToInteger(m_Config->Get("notify_cooldown_sec"))) * 1000;
  m_StatusNotify = (m_Config->Get("status_notify") == "1");
  m_CatchupMaxAgeMs = std::max<int64_t>(0, StrUtil::ToInteger(m_Config->Get("catchup_max_age_sec"))) * 1000;

  // entries are "profileid:chatid", "chatid" (any profile) or "*" (all chats)
  for (const std::string& entry : SplitList(m_Config->Get("watch_chats")))
  {
    if (entry == "*")
    {
      m_WatchAll = true;
    }
    else if (entry.find(':') != std::string::npos)
    {
      m_WatchKeys.insert(entry);
    }
    else
    {
      m_WatchChatIds.insert(entry);
    }
  }
}

void Ui::LoadState()
{
  std::ifstream stream(m_StatePath);
  std::string line;
  while (std::getline(stream, line))
  {
    // key is "profileid:chatid", value is message time in ms
    const size_t sep = line.rfind('=');
    if (sep == std::string::npos) continue;

    const std::string value = line.substr(sep + 1);
    if (!StrUtil::IsInteger(value)) continue;

    m_Watermarks[line.substr(0, sep)] = std::stoll(value);
  }

  LOG_DEBUG("loaded %d watermark(s)", (int)m_Watermarks.size());
}

void Ui::SaveState()
{
  std::ostringstream sstream;
  for (const auto& watermark : m_Watermarks)
  {
    sstream << watermark.first << "=" << watermark.second << "\n";
  }

  // write and rename, so a crash never leaves a truncated state file
  const std::string tmpPath = m_StatePath + ".tmp";
  FileUtil::WriteFile(tmpPath, sstream.str());
  if (rename(tmpPath.c_str(), m_StatePath.c_str()) != 0)
  {
    LOG_WARNING("failed to save %s", m_StatePath.c_str());
  }

  m_StateDirty = false;
}

void Ui::SaveChatList()
{
  std::ostringstream sstream;
  sstream << "# chats seen by nchat-notify, use the first column in watch_chats (notifier.conf)\n";
  for (const auto& profileChats : m_ChatIds)
  {
    for (const std::string& chatId : profileChats.second)
    {
      sstream << GetKey(profileChats.first, chatId) << "\t" << GetName(profileChats.first, chatId) << "\n";
    }
  }

  FileUtil::WriteFile(m_ChatListPath, sstream.str());
  m_ChatListDirty = false;
}

void Ui::HandleConnectNotify(std::shared_ptr<ConnectNotify> p_ConnectNotify)
{
  const std::string& profileId = p_ConnectNotify->profileId;
  if (!p_ConnectNotify->success)
  {
    Alert("login failed for " + profileId);
    NotifyStatus("Login failed for " + profileId + ", check the server.");
    return;
  }

  Notice("connected " + profileId);

  // request chats and contacts to populate the chat list file and resolve names,
  // but never request anything that changes state (online status, read receipts)
  auto it = m_Protocols.find(profileId);
  if (it == m_Protocols.end()) return;

  std::shared_ptr<Protocol> protocol = it->second;
  if (!protocol->HasFeature(FeatureAutoGetChatsOnLogin))
  {
    protocol->SendRequest(std::make_shared<GetChatsRequest>());
  }

  if (!protocol->HasFeature(FeatureAutoGetContactsOnLogin))
  {
    protocol->SendRequest(std::make_shared<GetContactsRequest>());
  }
}

void Ui::HandleNewMessagesNotify(std::shared_ptr<NewMessagesNotify> p_NewMessagesNotify)
{
  // cached messages come from the local db, never from the network
  if (!p_NewMessagesNotify->success || p_NewMessagesNotify->cached) return;

  const std::string& profileId = p_NewMessagesNotify->profileId;
  const std::string& chatId = p_NewMessagesNotify->chatId;
  m_ChatListDirty |= m_ChatIds[profileId].insert(chatId).second;

  if (!IsWatched(profileId, chatId)) return;

  const std::string key = GetKey(profileId, chatId);
  const int64_t baseline = GetBaseline(key);
  std::set<std::string>& notifiedMsgIds = m_NotifiedMsgIds[key];
  if (notifiedMsgIds.size() > s_MaxNotifiedMsgIds)
  {
    notifiedMsgIds.clear();
  }

  const ChatMessage* newest = nullptr;
  for (const ChatMessage& chatMessage : p_NewMessagesNotify->chatMessages)
  {
    if (chatMessage.isOutgoing || chatMessage.isEdited || chatMessage.isDeleted) continue;

    // sponsored messages (tgchat) have max time
    if (chatMessage.timeSent == std::numeric_limits<int64_t>::max()) continue;

    // history fetched after connect, or already notified in a previous session
    if (chatMessage.timeSent <= baseline) continue;

    if (!notifiedMsgIds.insert(chatMessage.id).second) continue;

    if ((newest == nullptr) || (chatMessage.timeSent > newest->timeSent))
    {
      newest = &chatMessage;
    }
  }

  if (newest == nullptr) return;

  int64_t& watermark = m_Watermarks[key];
  if (newest->timeSent > watermark)
  {
    watermark = newest->timeSent;
    m_StateDirty = true;
  }

  const int64_t now = TimeUtil::GetCurrentTimeMSec();
  int64_t& lastNotifyTime = m_LastNotifyTime[key];
  if ((m_CooldownMs > 0) && ((now - lastNotifyTime) < m_CooldownMs))
  {
    LOG_DEBUG("cooldown, skip notify %s", key.c_str());
    return;
  }

  lastNotifyTime = now;
  NotifyMessage(profileId, chatId, *newest);
  m_CondVar.notify_all();
}

void Ui::HandleRequestAppExitNotify(std::shared_ptr<RequestAppExitNotify> p_RequestAppExitNotify)
{
  const std::string& profileId = p_RequestAppExitNotify->profileId;
  Alert("exit requested by " + profileId + ", the session may need to be set up again");
  NotifyStatus("Session closed for " + profileId + ", nchat-notify stopped. Re-run setup on the server.");
  m_ExitRequested = true;
  m_CondVar.notify_all();
}

bool Ui::IsWatched(const std::string& p_ProfileId, const std::string& p_ChatId) const
{
  return m_WatchAll || m_WatchChatIds.count(p_ChatId) || m_WatchKeys.count(GetKey(p_ProfileId, p_ChatId));
}

int64_t Ui::GetBaseline(const std::string& p_Key)
{
  auto it = m_Baselines.find(p_Key);
  if (it != m_Baselines.end()) return it->second;

  // without a watermark, start from now to skip the history fetched at login;
  // with one, catch up on messages missed while stopped, but limit how far back
  int64_t baseline = m_StartTime;
  auto wit = m_Watermarks.find(p_Key);
  if (wit != m_Watermarks.end())
  {
    baseline = std::max(wit->second, m_StartTime - m_CatchupMaxAgeMs);
  }

  m_Baselines[p_Key] = baseline;
  return baseline;
}

std::string Ui::GetName(const std::string& p_ProfileId, const std::string& p_Id) const
{
  auto pit = m_ContactNames.find(p_ProfileId);
  if (pit != m_ContactNames.end())
  {
    auto it = pit->second.find(p_Id);
    if ((it != pit->second.end()) && !it->second.empty()) return it->second;
  }

  return p_Id;
}

std::string Ui::Expand(const std::string& p_Template, const std::string& p_ProfileId, const std::string& p_ChatId,
                       const ChatMessage& p_ChatMessage) const
{
  // single pass, so placeholders inside substituted values (e.g. message text) are kept as-is
  std::string rv;
  size_t pos = 0;
  while (pos < p_Template.size())
  {
    const size_t start = p_Template.find('%', pos);
    const size_t end = (start == std::string::npos) ? std::string::npos : p_Template.find('%', start + 1);
    if (end == std::string::npos)
    {
      rv += p_Template.substr(pos);
      break;
    }

    rv += p_Template.substr(pos, start - pos);
    const std::string name = p_Template.substr(start + 1, end - start - 1);
    if (name == "profile")
    {
      rv += p_ProfileId;
    }
    else if (name == "protocol")
    {
      rv += StrUtil::GetProtocolName(p_ProfileId);
    }
    else if (name == "chat")
    {
      rv += GetName(p_ProfileId, p_ChatId);
    }
    else if (name == "chatid")
    {
      rv += p_ChatId;
    }
    else if (name == "sender")
    {
      rv += GetName(p_ProfileId, p_ChatMessage.senderId);
    }
    else if (name == "text")
    {
      const std::string text = p_ChatMessage.text.empty() && !p_ChatMessage.fileInfo.empty()
        ? "[Attachment]" : p_ChatMessage.text;
      rv += TruncateUtf8(text, s_MaxTextLen);
    }
    else
    {
      // not a placeholder, keep the first '%' and rescan from the second
      rv += "%";
      pos = start + 1;
      continue;
    }

    pos = end + 1;
  }

  return rv;
}

void Ui::NotifyMessage(const std::string& p_ProfileId, const std::string& p_ChatId, const ChatMessage& p_ChatMessage)
{
  NtfyMessage message;
  message.topic = m_Topic;
  message.title = Expand(m_Title, p_ProfileId, p_ChatId, p_ChatMessage);
  message.message = Expand(m_Message, p_ProfileId, p_ChatId, p_ChatMessage);
  message.priority = m_Priority;
  message.tags = m_Tags;
  message.click = m_Click;

  LOG_INFO("notify new message in %s", GetKey(p_ProfileId, p_ChatId).c_str());
  m_Ntfy.Publish(message);
}

void Ui::NotifyStatus(const std::string& p_Text)
{
  if (!m_StatusNotify || m_StatusTopic.empty()) return;

  NtfyMessage message;
  message.topic = m_StatusTopic;
  message.title = "nchat-notify";
  message.message = p_Text;
  message.priority = 4;
  message.tags = std::vector<std::string>({ "warning" });
  m_Ntfy.Publish(message);
}

std::string Ui::GetKey(const std::string& p_ProfileId, const std::string& p_ChatId)
{
  return p_ProfileId + ":" + p_ChatId;
}
