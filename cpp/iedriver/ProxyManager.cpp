// Licensed to the Software Freedom Conservancy (SFC) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership. The SFC licenses this file
// to you under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ProxyManager.h"
#include <algorithm>
#include <vector>
#include <wininet.h>
#include "json.h"
#include "logging.h"
#include "messages.h"
#include "HookProcessor.h"

#define WD_PROXY_TYPE_DIRECT "direct"
#define WD_PROXY_TYPE_SYSTEM "system"
#define WD_PROXY_TYPE_MANUAL "manual"
#define WD_PROXY_TYPE_AUTOCONFIGURE "pac"
#define WD_PROXY_TYPE_AUTODETECT "autodetect"

namespace webdriver {

ProxyManager::ProxyManager(void) {
}

ProxyManager::~ProxyManager(void) {
  this->RestoreProxySettings();
}

void ProxyManager::Initialize(ProxySettings settings) {
  LOG(TRACE) << "ProxyManager::Initialize";
  // The wire protocol specifies lower case for the proxy type, but
  // language bindings have been sending upper case forever. Handle
  // both cases, thus we will normalize to a lower-case string for
  // proxy type.
  this->proxy_type_ = settings.proxy_type;
  std::transform(this->proxy_type_.begin(),
                 this->proxy_type_.end(),
                 this->proxy_type_.begin(),
                 ::tolower);
  this->http_proxy_ = settings.http_proxy;
  this->ftp_proxy_ = settings.ftp_proxy;
  this->ssl_proxy_ = settings.ssl_proxy;
  this->proxy_autoconfigure_url_ = settings.proxy_autoconfig_url;
  this->is_proxy_modified_ = false;
  if (this->proxy_type_ == WD_PROXY_TYPE_SYSTEM ||
      this->proxy_type_ == WD_PROXY_TYPE_DIRECT ||
      this->proxy_type_ == WD_PROXY_TYPE_MANUAL) {
    this->use_per_process_proxy_ = settings.use_per_process_proxy;
  } else {
    // By definition, per-process proxy settings can only be used with the
    // system proxy, direct connection, or with a manually specified proxy.
    this->use_per_process_proxy_ = false;
  }

  this->current_autoconfig_url_ = L"";
  this->current_proxy_auto_detect_flags_ = 0;
  this->current_proxy_server_ = L"";
  this->current_proxy_type_ = 0;
  this->current_proxy_bypass_list_ = L"";
}

void ProxyManager::SetProxySettings(HWND browser_window_handle) {
  LOG(TRACE) << "ProxyManager::SetProxySettings";
  if (this->proxy_type_.size() > 0 && this->proxy_type_ != WD_PROXY_TYPE_SYSTEM) {
    if (this->use_per_process_proxy_) {
      LOG(DEBUG) << "Setting proxy for individual IE instance.";
      this->SetPerProcessProxySettings(browser_window_handle);
    } else {
      if (!this->is_proxy_modified_) {
        LOG(DEBUG) << "Setting system proxy.";
        this->GetCurrentProxySettings();
        this->SetGlobalProxySettings();
      } else {
        LOG(DEBUG) << "Proxy settings already set by IE driver.";
      }
    }
    this->is_proxy_modified_ = true;
  } else {
    LOG(DEBUG) << "Using existing system proxy settings.";
  }
}

Json::Value ProxyManager::GetProxyAsJson() {
  LOG(TRACE) << "ProxyManager::GetProxyAsJson";
  Json::Value proxy_value;
  proxy_value["proxyType"] = this->proxy_type_;
  if (this->proxy_type_ == WD_PROXY_TYPE_MANUAL) {
    if (this->http_proxy_.size() > 0) {
      proxy_value["httpProxy"] = this->http_proxy_;
    }
    if (this->ftp_proxy_.size() > 0) {
      proxy_value["ftpProxy"] = this->ftp_proxy_;
    }
    if (this->ssl_proxy_.size() > 0) {
      proxy_value["sslProxy"] = this->ssl_proxy_;
    }
  } else if (this->proxy_type_ == WD_PROXY_TYPE_AUTOCONFIGURE) {
    proxy_value["proxyAutoconfigUrl"] = this->proxy_autoconfigure_url_;
  }
  return proxy_value;
}

std::wstring ProxyManager::BuildProxySettingsString() {
  LOG(TRACE) << "ProxyManager::BuildProxySettingsString";
  std::string proxy_string = "";
  if (this->proxy_type_ == WD_PROXY_TYPE_MANUAL) {
    if (this->http_proxy_.size() > 0) {
      proxy_string.append("http=").append(this->http_proxy_);
    }
    if (this->ftp_proxy_.size() > 0) {
      if (proxy_string.size() > 0) {
        proxy_string.append(" ");
      }
      proxy_string.append("ftp=").append(this->ftp_proxy_);
    }
    if (this->ssl_proxy_.size() > 0) {
      if (proxy_string.size() > 0) {
        proxy_string.append(" ");
      }
      proxy_string.append("https=").append(this->ssl_proxy_);
    }
  } else if (this->proxy_type_ == WD_PROXY_TYPE_AUTOCONFIGURE) {
    proxy_string = this->proxy_autoconfigure_url_;
  } else {
    proxy_string = this->proxy_type_;
  }
  LOG(DEBUG) << "Built proxy settings string: '" << proxy_string << "'";
  return StringUtilities::ToWString(proxy_string);
}

void ProxyManager::RestoreProxySettings() {
  LOG(TRACE) << "ProxyManager::RestoreProxySettings";
  if (!this->use_per_process_proxy_ && this->is_proxy_modified_) {
    INTERNET_PER_CONN_OPTION_LIST option_list;
    std::vector<INTERNET_PER_CONN_OPTION> restore_options(5);
    unsigned long list_size = sizeof(INTERNET_PER_CONN_OPTION_LIST);

    std::vector<wchar_t> autoconfig_url_buffer;
    StringUtilities::ToBuffer(this->current_autoconfig_url_, &autoconfig_url_buffer);
    restore_options[0].dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL;
    restore_options[0].Value.pszValue = &autoconfig_url_buffer[0];

    restore_options[1].dwOption = INTERNET_PER_CONN_AUTODISCOVERY_FLAGS;
    restore_options[1].Value.dwValue = this->current_proxy_auto_detect_flags_;

    restore_options[2].dwOption = INTERNET_PER_CONN_FLAGS;
    restore_options[2].Value.dwValue = this->current_proxy_type_;

    std::vector<wchar_t> proxy_bypass_buffer;
    StringUtilities::ToBuffer(this->current_proxy_bypass_list_, &proxy_bypass_buffer);
    restore_options[3].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
    restore_options[3].Value.pszValue = &proxy_bypass_buffer[0];

    std::vector<wchar_t> proxy_server_buffer;
    StringUtilities::ToBuffer(this->current_proxy_server_, &proxy_server_buffer);
    restore_options[4].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
    restore_options[4].Value.pszValue = &proxy_server_buffer[0];

    option_list.dwSize = sizeof(INTERNET_PER_CONN_OPTION_LIST);
    option_list.pszConnection = NULL;
    option_list.dwOptionCount = static_cast<int>(restore_options.size());
    option_list.dwOptionError = 0;
    option_list.pOptions = &restore_options[0];

    BOOL success = ::InternetSetOption(NULL,
                                       INTERNET_OPTION_PER_CONNECTION_OPTION,
                                       &option_list,
                                       list_size);
    if (!success) {
      LOGERR(WARN) << "InternetSetOption failed setting INTERNET_OPTION_PER_CONNECTION_OPTION";
    }
    success = ::InternetSetOption(NULL,
                                  INTERNET_OPTION_PROXY_SETTINGS_CHANGED,
                                  NULL,
                                  0);
    this->is_proxy_modified_ = false;
    if (!success) {
      LOGERR(WARN) << "InternetSetOption failed setting INTERNET_OPTION_PROXY_SETTINGS_CHANGED";
    }
  }
}

void ProxyManager::SetPerProcessProxySettings(HWND browser_window_handle) {
  LOG(TRACE) << "ProxyManager::SetPerProcessProxySettings";
  std::wstring proxy = this->BuildProxySettingsString();
  HookProcessor hook(browser_window_handle);
  hook.InstallWindowsHook("SetProxyWndProc", WH_CALLWNDPROC);
  hook.PushData(static_cast<int>(proxy.size() * sizeof(wchar_t)), &proxy[0]);
  LRESULT result = ::SendMessage(browser_window_handle,
                                 WD_CHANGE_PROXY,
                                 NULL,
                                 NULL);
  LOG(INFO) << "SendMessage result? " << result;
  hook.UninstallWindowsHook();
}

void ProxyManager::SetGlobalProxySettings() {
  LOG(TRACE) << "ProxyManager::SetGlobalProxySettings";
  std::wstring proxy = this->BuildProxySettingsString();

  INTERNET_PER_CONN_OPTION_LIST option_list;
  unsigned long list_size = sizeof(INTERNET_PER_CONN_OPTION_LIST);
  option_list.dwSize = sizeof(INTERNET_PER_CONN_OPTION_LIST);

  INTERNET_PER_CONN_OPTION proxy_options[3];
  if (this->proxy_type_ == WD_PROXY_TYPE_DIRECT) {
    proxy_options[0].dwOption = INTERNET_PER_CONN_FLAGS;
    proxy_options[0].Value.dwValue = PROXY_TYPE_DIRECT;
    option_list.dwOptionCount = 1;
  } else if (this->proxy_type_ == WD_PROXY_TYPE_AUTOCONFIGURE) {
    proxy_options[0].dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL;
    proxy_options[0].Value.pszValue = const_cast<wchar_t*>(proxy.c_str());
    proxy_options[1].dwOption = INTERNET_PER_CONN_FLAGS;
    proxy_options[1].Value.dwValue = PROXY_TYPE_AUTO_PROXY_URL;
    option_list.dwOptionCount = 2;
  } else if (this->proxy_type_ == WD_PROXY_TYPE_AUTODETECT) {
    proxy_options[0].dwOption = INTERNET_PER_CONN_AUTODISCOVERY_FLAGS;
    proxy_options[0].Value.dwValue = AUTO_PROXY_FLAG_ALWAYS_DETECT;
    proxy_options[1].dwOption = INTERNET_PER_CONN_FLAGS;
    proxy_options[1].Value.dwValue = PROXY_TYPE_AUTO_DETECT;
    option_list.dwOptionCount = 2;
  } else {
    proxy_options[0].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
    proxy_options[0].Value.pszValue = const_cast<wchar_t*>(proxy.c_str());
    proxy_options[1].dwOption = INTERNET_PER_CONN_FLAGS;
    proxy_options[1].Value.dwValue = PROXY_TYPE_PROXY | PROXY_TYPE_DIRECT;
    proxy_options[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
    proxy_options[2].Value.pszValue = L"";
    option_list.dwOptionCount = 3;
  }

  option_list.pOptions = proxy_options;
  option_list.pszConnection = NULL;
  option_list.dwOptionError = 0;
  BOOL success = ::InternetSetOption(NULL,
                                     INTERNET_OPTION_PER_CONNECTION_OPTION,
                                     &option_list,
                                     list_size);
  if (!success) {
    LOGERR(WARN) << "InternetSetOption failed setting INTERNET_OPTION_PER_CONNECTION_OPTION";
  }
  success = ::InternetSetOption(NULL,
                                INTERNET_OPTION_PROXY_SETTINGS_CHANGED,
                                NULL,
                                0);
  if (!success) {
    LOGERR(WARN) << "InternetSetOption failed setting INTERNET_OPTION_PROXY_SETTINGS_CHANGED";
  }
}

void ProxyManager::GetCurrentProxySettings() {
  LOG(TRACE) << "ProxyManager::GetCurrentProxySettings";
  this->GetCurrentProxyType();
  INTERNET_PER_CONN_OPTION_LIST option_list;
  std::vector<INTERNET_PER_CONN_OPTION> options_to_get(4);
  unsigned long list_size = sizeof(INTERNET_PER_CONN_OPTION_LIST);

  options_to_get[0].dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL;
  options_to_get[1].dwOption = INTERNET_PER_CONN_AUTODISCOVERY_FLAGS;
  options_to_get[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
  options_to_get[3].dwOption = INTERNET_PER_CONN_PROXY_SERVER;

  option_list.dwSize = sizeof(INTERNET_PER_CONN_OPTION_LIST);
  option_list.pszConnection = NULL;
  option_list.dwOptionCount = static_cast<int>(options_to_get.size());
  option_list.dwOptionError = 0;
  option_list.pOptions = &options_to_get[0];

  BOOL success = ::InternetQueryOption(NULL,
                                       INTERNET_OPTION_PER_CONNECTION_OPTION,
                                       &option_list,
                                       &list_size);
  if (!success) {
    LOGERR(WARN) << "InternetQueryOption failed getting proxy settings";
  }

  if(options_to_get[0].Value.pszValue != NULL) {
    this->current_autoconfig_url_ = options_to_get[0].Value.pszValue;
    ::GlobalFree(options_to_get[0].Value.pszValue);
  }

  this->current_proxy_auto_detect_flags_ = options_to_get[1].Value.dwValue;

  if(options_to_get[2].Value.pszValue != NULL) {
    this->current_proxy_bypass_list_ = options_to_get[2].Value.pszValue;
    ::GlobalFree(options_to_get[2].Value.pszValue);
  }

  if(options_to_get[3].Value.pszValue != NULL) {
    this->current_proxy_server_ = options_to_get[3].Value.pszValue;
    ::GlobalFree(options_to_get[3].Value.pszValue);
  }
}

void ProxyManager::GetCurrentProxyType() {
  LOG(TRACE) << "ProxyManager::GetCurrentProxyType";
  INTERNET_PER_CONN_OPTION_LIST option_list;
  std::vector<INTERNET_PER_CONN_OPTION> proxy_type_options(1);
  unsigned long list_size = sizeof(INTERNET_PER_CONN_OPTION_LIST);

  proxy_type_options[0].dwOption = INTERNET_PER_CONN_FLAGS_UI;

  option_list.dwSize = sizeof(INTERNET_PER_CONN_OPTION_LIST);
  option_list.pszConnection = NULL;
  option_list.dwOptionCount = static_cast<int>(proxy_type_options.size());
  option_list.dwOptionError = 0;
  option_list.pOptions = &proxy_type_options[0];

  // First check for INTERNET_PER_CONN_FLAGS_UI, then if that fails
  // check again using INTERNET_PER_CONN_FLAGS. This is documented at
  // http://msdn.microsoft.com/en-us/library/windows/desktop/aa385145%28v=vs.85%29.aspx
  BOOL success = ::InternetQueryOption(NULL,
                                       INTERNET_OPTION_PER_CONNECTION_OPTION,
                                       &option_list,
                                       &list_size);
  if (success) {
    this->current_proxy_type_ = proxy_type_options[0].Value.dwValue;
    return;
  }

  proxy_type_options[0].dwOption = INTERNET_PER_CONN_FLAGS;
  success = ::InternetQueryOption(NULL,
                                  INTERNET_OPTION_PER_CONNECTION_OPTION,
                                  &option_list,
                                  &list_size);
  if (success) {
    this->current_proxy_type_ = proxy_type_options[0].Value.dwValue;
  }
}

} // namespace webdriver

#ifdef __cplusplus
extern "C" {
#endif

LRESULT CALLBACK SetProxyWndProc(int nCode, WPARAM wParam, LPARAM lParam) {
  CWPSTRUCT* call_window_proc_struct = reinterpret_cast<CWPSTRUCT*>(lParam);
  if (WM_COPYDATA == call_window_proc_struct->message) {
    COPYDATASTRUCT* data = reinterpret_cast<COPYDATASTRUCT*>(call_window_proc_struct->lParam);
    webdriver::HookProcessor::CopyDataToBuffer(data->cbData, data->lpData);
  } else if (WD_CHANGE_PROXY == call_window_proc_struct->message) {
    // Allocate a buffer of wchar_t the length of the data in the
    // shared memory buffer, plus one extra wide char, so that we
    // can null terminate.
    int proxy_string_buffer_size = webdriver::HookProcessor::GetDataBufferSize() + sizeof(wchar_t);
    std::vector<wchar_t> proxy_string_buffer(proxy_string_buffer_size / sizeof(wchar_t));

    // Copy the data from the shared memory buffer, and force
    // a terminating null char into the local vector, then 
    // convert to wstring, so it's easier to work with.
    webdriver::HookProcessor::CopyDataFromBuffer(proxy_string_buffer_size,
                                                 &proxy_string_buffer[0]);
    proxy_string_buffer[proxy_string_buffer.size() - 1] = L'\0';
    std::wstring proxy = &proxy_string_buffer[0];

    INTERNET_PROXY_INFO proxy_info;
    std::vector<char> multibyte_buffer(proxy_string_buffer_size);
    if (proxy == L"direct") {
      proxy_info.dwAccessType = INTERNET_OPEN_TYPE_DIRECT;
      proxy_info.lpszProxy = L"";
    } else {
      // UrlMkSetSessionOption only appears to work on either ASCII or
      // multi-byte strings, not Unicode strings. Since the INTERNET_PROXY_INFO
      // struct hard-codes to LPCTSTR, and that translates into LPCWSTR for the
      // compiler settings we use, we must use the multi-byte version here.
      // Note that for the count of input characters, we can use -1, since
      // we've forced the string to be null-terminated.
      ::WideCharToMultiByte(CP_UTF8,
                            0,
                            proxy.c_str(),
                            -1,
                            &multibyte_buffer[0],
                            static_cast<int>(multibyte_buffer.size()),
                            NULL,
                            NULL);
      proxy_info.dwAccessType = INTERNET_OPEN_TYPE_PROXY;
      proxy_info.lpszProxy = reinterpret_cast<LPCTSTR>(&multibyte_buffer[0]);
    }
    proxy_info.lpszProxyBypass = L"";
    DWORD proxy_info_size = sizeof(proxy_info);
    HRESULT hr = ::UrlMkSetSessionOption(INTERNET_OPTION_PROXY,
                                         reinterpret_cast<void*>(&proxy_info),
                                         proxy_info_size,
                                         0);
  }

  //return ::CallNextHookEx(window_proc_hook, nCode, wParam, lParam);
  return ::CallNextHookEx(NULL, nCode, wParam, lParam);
}

#ifdef __cplusplus
}
#endif
