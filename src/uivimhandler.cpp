// uivimhandler.cpp
//
// Copyright (c) 2019-2026 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.
#include "uimodel.h"

#include <algorithm>

#include <ncurses.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "appconfig.h"
#include "messagecache.h"
#include "apputil.h"
#include "clipboard.h"
#include "composescript.h"
#include "fileutil.h"
#include "log.h"
#include "numutil.h"
#include "protocolutil.h"
#include "sethelp.h"
#include "status.h"
#include "strutil.h"
#include "sysutil.h"
#include "timeutil.h"
#include "uidialog.h"
#include "uichatlistdialog.h"
#include "uiconfig.h"
#include "uicontactlistdialog.h"
#include "uigroupmemberlistdialog.h"
#include "uicontroller.h"
#include "uiemojilistdialog.h"
#include "uifilelistdialog.h"
#include "uikeyconfig.h"
#include "uikeyinput.h"
#include "uimessagedialog.h"
#include "uitextinputdialog.h"
#include "uiview.h"
#include "uiviewbase.h"
#include "uimodel.h"

std::wstring m_VimUndoStr;
int m_VimUndoPos = 0;
bool m_VimUndoValid = false;

static void SetCursorStyle(int p_Style)
{
  std::string seq = "\033[" + std::to_string(p_Style) + " q";
  fputs(seq.c_str(), stdout);
  fflush(stdout);   // important — ncurses buffers, you need this to flush now
}

bool UiModel::Impl::vimEnabled() {
  m_vimEnabled = UiConfig::GetBool("vim_enabled");
  return m_vimEnabled;
}
bool UiModel::Impl::VimActiveNonInsert() {
  if (!vimEnabled() || m_VimMode == VimInsert) {
    return false;
  }
  return true;
}

bool UiModel::Impl::VimNormalFeed(wint_t p_Key) {
  std::string profileId = m_CurrentChat.first;
  std::string chatId = m_CurrentChat.second;
  int &entryPos = m_EntryPos[profileId][chatId];
  std::wstring &entryStr = m_EntryStr[profileId][chatId];

  const int messageCount = m_Messages[profileId][chatId].size();
  int &messageOffset = m_MessageOffset[profileId][chatId];

  if (p_Key == 'K') {
    if (GetSelectMessageActive() && !GetEditMessageActive()) {
      messageOffset = std::min(messageOffset + 1, messageCount - 1);
      RequestMessagesCurrentChat();
    }
  } else if (p_Key == 'k') {
    if ((entryPos == 0) && (messageCount > 0) && !GetEditMessageActive()) {
      SetSelectMessageActive(true);
    } else {
      int cx = 0;
      int cy = 0;
      int width = m_View->GetEntryWidth();
      std::vector<std::wstring> lines = StrUtil::WordWrap(
          entryStr, width, false, false, false, 2, entryPos, cy, cx);
      if (cy > 0) {
        int stepsBack = 0;
        int prevLineLen = lines.at(cy - 1).size();
        if (prevLineLen > cx) {
          stepsBack = prevLineLen + 1;
        } else {
          stepsBack = cx + 1;
        }

        stepsBack = std::min(stepsBack, width);
        entryPos =
            NumUtil::Bound(0, entryPos - stepsBack, (int)entryStr.size());

        if ((entryPos < (int)entryStr.size()) &&
            (entryStr.at(entryPos) == (wchar_t)EMOJI_PAD)) {
          entryPos = NumUtil::Bound(0, entryPos - 1, (int)entryStr.size());
        }
      } else {
        entryPos = 0;
      }
    }
  } else if (p_Key == 'J') {
    if (GetSelectMessageActive() && !GetEditMessageActive()) {
      if (messageOffset > 0) {
        messageOffset = messageOffset - 1;
      } else {
        SetSelectMessageActive(false);
      }
    }
  } else if (p_Key == 'j') {
    if (entryPos < (int)entryStr.size()) {
      int cx = 0;
      int cy = 0;
      int width = m_View->GetEntryWidth();
      std::vector<std::wstring> lines = StrUtil::WordWrap(
          entryStr, width, false, false, false, 2, entryPos, cy, cx);

      int stepsForward = (int)lines.at(cy).size() - cx + 1;
      if ((cy + 1) < (int)lines.size()) {
        if ((int)lines.at(cy + 1).size() > cx) {
          stepsForward += cx;
        } else {
          stepsForward += lines.at(cy + 1).size();
        }
      }

      stepsForward = std::min(stepsForward, width);
      entryPos =
          NumUtil::Bound(0, entryPos + stepsForward, (int)entryStr.size());

      if ((entryPos < (int)entryStr.size()) &&
          (entryStr.at(entryPos) == (wchar_t)EMOJI_PAD)) {
        entryPos = NumUtil::Bound(0, entryPos - 1, (int)entryStr.size());
      }
    }
  } else if (p_Key == 'h') {
    entryPos = NumUtil::Bound(0, entryPos - 1, (int)entryStr.size());
    if ((entryPos < (int)entryStr.size()) &&
        (entryStr.at(entryPos) == (wchar_t)EMOJI_PAD)) {
      entryPos = NumUtil::Bound(0, entryPos - 1, (int)entryStr.size());
    }
  } else if (p_Key == 'l') {
    entryPos = NumUtil::Bound(0, entryPos + 1, (int)entryStr.size());
    if ((entryPos < (int)entryStr.size()) &&
        (entryStr.at(entryPos) == (wchar_t)EMOJI_PAD)) {
      entryPos = NumUtil::Bound(0, entryPos + 1, (int)entryStr.size());
    }
  } else if (p_Key == 'I') {
    UiModel::Impl::VimEnterInsert(-1);
  } else if (p_Key == 'i') {
    UiModel::Impl::VimEnterInsert(0);
  } else if (p_Key == 'a') {
    UiModel::Impl::VimEnterInsert(1);
  } else if (p_Key == 'A') {
    UiModel::Impl::VimEnterInsert(2);
  } else if (p_Key == 'o') {
    int len = (int)entryStr.size();
    size_t eol = entryStr.find(L'\n', entryPos);
    int insertAt = (eol == std::wstring::npos) ? len : (int)eol;

    entryStr.insert(entryStr.begin() + insertAt, L'\n');
    entryPos = insertAt + 1;
    UiModel::Impl::VimEnterInsert(0);
    SetTyping(profileId, chatId, true);
  } else if (p_Key == 'O') {
    size_t bol = (entryPos == 0) ? std::wstring::npos
                                 : entryStr.rfind(L'\n', entryPos - 1);
    int insertAt = (bol == std::wstring::npos) ? 0 : (int)bol + 1;  

    entryStr.insert(entryStr.begin() + insertAt, L'\n');
    entryPos = insertAt;       // cursor on the new empty line, above the old text
    UiModel::Impl::VimEnterInsert(0);
    SetTyping(profileId, chatId, true);
  } else if (p_Key == 'w') {
    int len = (int)entryStr.size();
    while (entryPos < len && !iswspace(entryStr[entryPos]))
      ++entryPos;
    while (entryPos < len && iswspace(entryStr[entryPos]))
      ++entryPos;
  } else if (p_Key == 'b') {
    while (entryPos > 0 && iswspace(entryStr[entryPos - 1]))
      --entryPos;
    while (entryPos > 0 && !iswspace(entryStr[entryPos - 1]))
      --entryPos;
  } else if (p_Key == 'd' || p_Key == 'y' || p_Key == 'c') {
    VimEnterOpPending(p_Key);
  } else if (p_Key == 'p') {
    if (!m_VimRegister.empty()) {
      int insertAt = entryPos;
      if (insertAt < (int)entryStr.size()) insertAt++;
      entryStr.insert(insertAt, m_VimRegister);
      entryPos = insertAt + (int)m_VimRegister.size() - 1; 
      entryPos = NumUtil::Bound(0, entryPos, (int)entryStr.size());
      SetTyping(profileId, chatId, true);
    }
  } else if (p_Key == 'x') {
    if (entryPos < (int)entryStr.size()) {
      int to = entryPos + 1;
      if (to < (int)entryStr.size() && entryStr[to] == (wchar_t)EMOJI_PAD) to++;
      VimDeleteRange(entryStr, entryPos, entryPos, to);
      SetTyping(profileId, chatId, true);
      UpdateEntry();
    }
  } else if (p_Key == 'u') {
    if (m_VimUndoValid) {
      entryStr = m_VimUndoStr;
      entryPos = NumUtil::Bound(0, m_VimUndoPos, (int)entryStr.size());
      m_VimUndoValid = false;
      SetTyping(profileId, chatId, true);
      UpdateEntry();
    }
  }
    else {
    return false;
  }
  UpdateHistory();
  return true;
}
bool UiModel::Impl::VimOpPendingFeed(wint_t p_Key) {
  std::string profileId = m_CurrentChat.first;
  std::string chatId = m_CurrentChat.second;
  int &entryPos = m_EntryPos[profileId][chatId];
  std::wstring &entryStr = m_EntryStr[profileId][chatId];

  if (m_VimAnchor == -1) {
    if (p_Key == 'w') {
      int endPos = entryPos;
      StrUtil::JumpToNextMatch(entryStr, endPos, 0, L" \n");
      VimDeleteRange(entryStr, entryPos, entryPos, endPos);
      SetTyping(profileId, chatId, true);
    } 
    else if (p_Key == 'b') {
      int startPos = entryPos;
      StrUtil::JumpToPrevMatch(entryStr, startPos, -1, L" \n");
      VimDeleteRange(entryStr, entryPos, startPos, entryPos);
      SetTyping(profileId, chatId, true);
    } 
    else if (p_Key == 'd') {
      VimDeleteRange(entryStr, entryPos, 0, (int) entryStr.size());
      SetTyping(profileId, chatId, true);
    } else if (p_Key == '$') {
      VimDeleteRange(entryStr, entryPos, entryPos, (int) entryStr.size());
      SetTyping(profileId, chatId, true);
    } else if (p_Key == '0') {
      VimDeleteRange(entryStr, entryPos, 0, entryPos);
      SetTyping(profileId, chatId, true);
    }
    if (p_Key == 'i') {
      m_VimAnchor = 'i';
    } else if (m_VimPendingOp == 'c') {
      VimEnterInsert(0);
      m_VimPendingOp = 0;
    } else {
      VimEnterNormal(0);
      m_VimPendingOp = 0;
    }
  } else if (m_VimAnchor == 'i') {
    if (p_Key == 'w') {
      int startPos = entryPos;
      int endPos = entryPos;
      while (startPos > 0 &&
             entryStr[startPos - 1] != L' ' && entryStr[startPos - 1] != L'\n')
        startPos--;
      while (endPos < (int)entryStr.size() &&
             entryStr[endPos] != L' ' && entryStr[endPos] != L'\n')
        endPos++;
      VimDeleteRange(entryStr, entryPos, startPos, endPos);
      SetTyping(profileId, chatId, true);
      m_VimAnchor = -1;
    }
    if (m_VimPendingOp == 'c') {
      VimEnterInsert(0);
    } else {
      VimEnterNormal(0);
    }
    m_VimPendingOp = 0;

  }
  else {
    return false;
  }
  return true;
}

void UiModel::Impl::VimDeleteRange(std::wstring& s, int& pos, int from, int to)
{
  if (from > to) std::swap(from, to);

  m_VimUndoStr = s;
  m_VimUndoPos = pos;
  m_VimUndoValid = true;
  m_VimRegister = s.substr(from, to - from);

  if (m_VimPendingOp != 'y') {
    s.erase(from, to - from);
    pos = NumUtil::Bound(0, from, (int)s.size());
  }
}

bool UiModel::Impl::VimFeed(wint_t p_Key) {
  if (m_VimMode == VimNormal) {
    VimNormalFeed(p_Key);
  } else if (m_VimMode == VimOpPending) {
    VimOpPendingFeed(p_Key);
  } else {
    return false;
  }


  UpdateEntry();
  return true;
}

void UiModel::Impl::VimEnterOpPending(int op) {
  m_VimPendingOp = op;
  m_VimMode = VimOpPending;
  UpdateStatus();
}
void UiModel::Impl::VimEnterInsert(int position) {
  std::string profileId = m_CurrentChat.first;
  std::string chatId = m_CurrentChat.second;
  int& entryPos = m_EntryPos[profileId][chatId];
  std::wstring& entryStr = m_EntryStr[profileId][chatId];
  size_t nl = entryStr.find(L'\n', entryPos);

  m_VimMode = VimInsert;
  if (entryPos > 0) {
    if (position == -1) {
      entryPos = 0;
    } else if (position == 2) {
      entryPos = (nl == std::wstring::npos) ? (int)entryStr.size() : (int)nl;
    } else {
      entryPos += (entryPos == (int) entryStr.size()) ? 0 : position;
    }
  }
  SetCursorStyle(6);
  UpdateStatus();
  UpdateEntry();
}

void UiModel::Impl::VimEnterNormal(int position) {
  std::string profileId = m_CurrentChat.first;
  std::string chatId = m_CurrentChat.second;
  int& entryPos = m_EntryPos[profileId][chatId];

  m_VimMode = VimNormal;
  if (position == -1) {
    if (entryPos > 0) entryPos--;
  }
  SetCursorStyle(2);
  UpdateStatus();
  UpdateEntry();
}

bool UiModel::Impl::VimInsertAwaitingNormal() {
  return (vimEnabled() && m_VimMode == VimInsert);
}
std::string UiModel::getVimStatus() {
  return GetImpl().getVimStatus();   // use whatever your Impl pointer is named
}
std::string UiModel::Impl::getVimStatus() {
  std::string vimStatus;
  switch (m_VimMode) {
    case VimMode::VimNormal:
      vimStatus = "Normal"; break;
    case VimMode::VimInsert:
      vimStatus = "Insert"; break;
    case VimMode::VimOpPending:
      switch (m_VimPendingOp) {
          case 'd': vimStatus = "+DELETE"; break;
          case 'y': vimStatus = "+YANK"; break;
          case 'c': vimStatus = "+CHANGE"; break;
          default: vimStatus = "+"; break;
      }
      break;
    default: break;
  }

  return vimStatus;
}
