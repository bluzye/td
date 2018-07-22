//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2018
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/LanguagePackManager.h"

#include "td/telegram/ConfigShared.h"
#include "td/telegram/Global.h"
#include "td/telegram/logevent/LogEvent.h"
#include "td/telegram/net/NetQueryDispatcher.h"
#include "td/telegram/Td.h"

#include "td/telegram/td_api.h"
#include "td/telegram/telegram_api.h"

#include "td/db/SqliteDb.h"
#include "td/db/SqliteKeyValue.h"

#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/Status.h"

#include <atomic>
#include <utility>

namespace td {

struct LanguagePackManager::PluralizedString {
  string zero_value_;
  string one_value_;
  string two_value_;
  string few_value_;
  string many_value_;
  string other_value_;
};

struct LanguagePackManager::Language {
  std::mutex mutex_;
  std::atomic<int32> version_{-1};
  bool has_get_difference_query_ = false;
  std::unordered_map<string, string> ordinary_strings_;
  std::unordered_map<string, PluralizedString> pluralized_strings_;
  std::unordered_set<string> deleted_strings_;
  SqliteKeyValue kv_;  // usages should be guarded by database_->mutex_
};

struct LanguagePackManager::LanguagePack {
  std::mutex mutex_;
  std::unordered_map<string, std::unique_ptr<Language>> languages_;
};

struct LanguagePackManager::LanguageDatabase {
  std::mutex mutex_;
  string path_;
  SqliteDb database_;
  std::unordered_map<string, std::unique_ptr<LanguagePack>> language_packs_;
};

LanguagePackManager::~LanguagePackManager() = default;

bool LanguagePackManager::check_language_pack_name(Slice name) {
  for (auto c : name) {
    if (c != '_' && !is_alpha(c)) {
      return false;
    }
  }
  return true;
}

bool LanguagePackManager::check_language_code_name(Slice name) {
  for (auto c : name) {
    if (!is_alpha(c)) {
      return false;
    }
  }
  return name.size() == 2 || name.empty();
}

static Result<SqliteDb> open_database(const string &path) {
  TRY_RESULT(database, SqliteDb::open_with_key(path, DbKey::empty()));
  TRY_STATUS(database.exec("PRAGMA synchronous=NORMAL"));
  TRY_STATUS(database.exec("PRAGMA temp_store=MEMORY"));
  TRY_STATUS(database.exec("PRAGMA encoding=\"UTF-8\""));
  TRY_STATUS(database.exec("PRAGMA journal_mode=WAL"));
  return std::move(database);
}

static int32 load_database_language_version(SqliteKeyValue *kv) {
  string str_version = kv->get("!version");
  if (str_version.empty()) {
    return -1;
  }

  return to_integer<int32>(str_version);
}

void LanguagePackManager::start_up() {
  std::lock_guard<std::mutex> lock(language_database_mutex_);
  manager_count_++;
  language_pack_ = G()->shared_config().get_option_string("language_pack");
  language_code_ = G()->shared_config().get_option_string("language_code");
  CHECK(check_language_pack_name(language_pack_));
  CHECK(check_language_code_name(language_code_));

  string database_path = G()->shared_config().get_option_string("language_database_path");
  auto it = language_databases_.find(database_path);
  if (it == language_databases_.end()) {
    SqliteDb database;
    if (!database_path.empty()) {
      auto r_database = open_database(database_path);
      if (r_database.is_error()) {
        LOG(ERROR) << "Can't open language database " << database_path << ": " << r_database.error();
        database_path = string();
        it = language_databases_.find(database_path);
      } else {
        database = r_database.move_as_ok();
      }
    }

    if (it == language_databases_.end()) {
      it = language_databases_.emplace(database_path, make_unique<LanguageDatabase>()).first;
      it->second->path_ = std::move(database_path);
      it->second->database_ = std::move(database);
    }
  }
  database_ = it->second.get();

  // TODO load language_pack_version from database
  LOG(INFO) << "Use language pack " << language_pack_ << " with language " << language_code_ << " with database "
            << database_->path_;
}

void LanguagePackManager::tear_down() {
  std::lock_guard<std::mutex> lock(language_database_mutex_);
  manager_count_--;
  if (manager_count_ == 0) {
    // can't clear language packs, because the may be accessed later using synchronous requests
    // LOG(INFO) << "Clear language packs";
    // language_databases_.clear();
  }
}

void LanguagePackManager::on_language_pack_changed() {
  auto new_language_pack = G()->shared_config().get_option_string("language_pack");
  if (new_language_pack == language_pack_) {
    return;
  }

  language_pack_ = std::move(new_language_pack);
  CHECK(check_language_pack_name(language_pack_));
  inc_generation();
}

void LanguagePackManager::on_language_code_changed() {
  auto new_language_code = G()->shared_config().get_option_string("language_code");
  if (new_language_code == language_code_) {
    return;
  }

  language_code_ = std::move(new_language_code);
  CHECK(check_language_code_name(language_code_));
  inc_generation();
}

void LanguagePackManager::on_language_pack_version_changed(int32 new_version) {
  Language *language = get_language(database_, language_pack_, language_code_);
  int32 version = language == nullptr ? static_cast<int32>(-1) : language->version_.load();
  if (version == -1) {
    return;
  }

  auto new_language_pack_version =
      new_version >= 0 ? new_version : G()->shared_config().get_option_integer("language_pack_version", -1);
  if (new_language_pack_version <= version) {
    return;
  }

  std::lock_guard<std::mutex> lock(language->mutex_);
  if (language->has_get_difference_query_) {
    return;
  }

  language->has_get_difference_query_ = true;
  auto request_promise =
      PromiseCreator::lambda([actor_id = actor_id(this), language_pack = language_pack_, language_code = language_code_,
                              from_version = version](Result<NetQueryPtr> r_query) mutable {
        auto r_result = fetch_result<telegram_api::langpack_getDifference>(std::move(r_query));
        if (r_result.is_error()) {
          send_closure(actor_id, &LanguagePackManager::on_failed_get_difference, std::move(language_pack),
                       std::move(language_code));
          return;
        }

        auto result = r_result.move_as_ok();
        LOG(INFO) << "Receive language pack difference for language " << result->lang_code_ << " from version "
                  << result->from_version_ << " with version " << result->version_ << " of size "
                  << result->strings_.size();
        LOG_IF(ERROR, result->lang_code_ != language_code)
            << "Receive strings for " << result->lang_code_ << " instead of " << language_code;
        LOG_IF(ERROR, result->from_version_ != from_version)
            << "Receive strings from " << result->from_version_ << " instead of " << from_version;
        send_closure(actor_id, &LanguagePackManager::on_get_language_pack_strings, std::move(language_pack),
                     std::move(language_code), result->version_, true, vector<string>(), std::move(result->strings_),
                     Promise<td_api::object_ptr<td_api::languagePackStrings>>());
      });
  send_with_promise(G()->net_query_creator().create(create_storer(telegram_api::langpack_getDifference(version))),
                    std::move(request_promise));
}

void LanguagePackManager::on_update_language_pack(tl_object_ptr<telegram_api::langPackDifference> difference) {
  LOG(INFO) << "Receive update language pack difference for language " << difference->lang_code_ << " from version "
            << difference->from_version_ << " with version " << difference->version_ << " of size "
            << difference->strings_.size();
  if (difference->lang_code_ != language_code_) {
    LOG(WARNING) << "Ignore difference for language " << difference->lang_code_;
    return;
  }

  Language *language = get_language(database_, language_pack_, language_code_);
  int32 version = language == nullptr ? static_cast<int32>(-1) : language->version_.load();
  if (difference->version_ <= version) {
    LOG(INFO) << "Skip applying already applied updates";
    return;
  }
  if (version == -1 || version < difference->from_version_) {
    LOG(INFO) << "Can't apply difference";
    return on_language_pack_version_changed(difference->version_);
  }

  on_get_language_pack_strings(language_pack_, std::move(difference->lang_code_), difference->version_, true,
                               vector<string>(), std::move(difference->strings_),
                               Promise<td_api::object_ptr<td_api::languagePackStrings>>());
}

void LanguagePackManager::inc_generation() {
  generation_++;
  G()->shared_config().set_option_empty("language_pack_version");

  // TODO preload language and load language_pack_version from database
}

LanguagePackManager::Language *LanguagePackManager::get_language(LanguageDatabase *database,
                                                                 const string &language_pack,
                                                                 const string &language_code) {
  std::unique_lock<std::mutex> lock(database->mutex_);
  auto it = database->language_packs_.find(language_pack);
  if (it == database->language_packs_.end()) {
    return nullptr;
  }
  LanguagePack *pack = it->second.get();
  lock.unlock();
  return get_language(pack, language_code);
}

LanguagePackManager::Language *LanguagePackManager::get_language(LanguagePack *language_pack,
                                                                 const string &language_code) {
  CHECK(language_pack != nullptr);
  std::lock_guard<std::mutex> lock(language_pack->mutex_);
  auto it = language_pack->languages_.find(language_code);
  if (it == language_pack->languages_.end()) {
    return nullptr;
  }
  return it->second.get();
}

static string get_database_table_name(const string &language_pack, const string &language_code) {
  return PSTRING() << "kv_" << language_pack << "_" << language_code;
}

LanguagePackManager::Language *LanguagePackManager::add_language(LanguageDatabase *database,
                                                                 const string &language_pack,
                                                                 const string &language_code) {
  std::lock_guard<std::mutex> packs_lock(database->mutex_);
  auto pack_it = database->language_packs_.find(language_pack);
  if (pack_it == database->language_packs_.end()) {
    pack_it = database->language_packs_.emplace(language_pack, make_unique<LanguagePack>()).first;
  }
  LanguagePack *pack = pack_it->second.get();

  std::lock_guard<std::mutex> languages_lock(pack->mutex_);
  auto code_it = pack->languages_.find(language_code);
  if (code_it == pack->languages_.end()) {
    code_it = pack->languages_.emplace(language_code, make_unique<Language>()).first;
    code_it->second->kv_
        .init_with_connection(database->database_.clone(), get_database_table_name(language_pack, language_code))
        .ensure();
  }
  return code_it->second.get();
}

bool LanguagePackManager::language_has_string_unsafe(Language *language, const string &key) {
  return language->ordinary_strings_.count(key) != 0 || language->pluralized_strings_.count(key) != 0 ||
         language->deleted_strings_.count(key) != 0;
}

bool LanguagePackManager::language_has_strings(Language *language, const vector<string> &keys) {
  if (language == nullptr) {
    return false;
  }

  std::lock_guard<std::mutex> lock(language->mutex_);
  if (keys.empty()) {
    return language->version_ != -1;
  }
  for (auto &key : keys) {
    if (!language_has_string_unsafe(language, key)) {
      return false;
    }
  }
  return true;
}

td_api::object_ptr<td_api::LanguagePackString> LanguagePackManager::get_language_pack_string_object(
    const std::pair<string, string> &str) {
  return td_api::make_object<td_api::languagePackStringValue>(str.first, str.second);
}

td_api::object_ptr<td_api::LanguagePackString> LanguagePackManager::get_language_pack_string_object(
    const std::pair<string, PluralizedString> &str) {
  return td_api::make_object<td_api::languagePackStringPluralized>(
      str.first, str.second.zero_value_, str.second.one_value_, str.second.two_value_, str.second.few_value_,
      str.second.many_value_, str.second.other_value_);
}

td_api::object_ptr<td_api::LanguagePackString> LanguagePackManager::get_language_pack_string_object(const string &str) {
  return td_api::make_object<td_api::languagePackStringDeleted>(str);
}

td_api::object_ptr<td_api::languagePackStrings> LanguagePackManager::get_language_pack_strings_object(
    Language *language, const vector<string> &keys) {
  CHECK(language != nullptr);

  std::lock_guard<std::mutex> lock(language->mutex_);
  vector<td_api::object_ptr<td_api::LanguagePackString>> strings;
  if (keys.empty()) {
    for (auto &str : language->ordinary_strings_) {
      strings.push_back(get_language_pack_string_object(str));
    }
    for (auto &str : language->pluralized_strings_) {
      strings.push_back(get_language_pack_string_object(str));
    }
  } else {
    for (auto &key : keys) {
      auto ordinary_it = language->ordinary_strings_.find(key);
      if (ordinary_it != language->ordinary_strings_.end()) {
        strings.push_back(get_language_pack_string_object(*ordinary_it));
        continue;
      }
      auto pluralized_it = language->pluralized_strings_.find(key);
      if (pluralized_it != language->pluralized_strings_.end()) {
        strings.push_back(get_language_pack_string_object(*pluralized_it));
        continue;
      }
      LOG_IF(ERROR, language->deleted_strings_.count(key) == 0) << "Have no string for key " << key;
      strings.push_back(get_language_pack_string_object(key));
    }
  }

  return td_api::make_object<td_api::languagePackStrings>(std::move(strings));
}

void LanguagePackManager::get_languages(Promise<td_api::object_ptr<td_api::languagePack>> promise) {
  auto request_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<NetQueryPtr> r_query) mutable {
    auto r_result = fetch_result<telegram_api::langpack_getLanguages>(std::move(r_query));
    if (r_result.is_error()) {
      return promise.set_error(r_result.move_as_error());
    }

    auto languages = r_result.move_as_ok();
    auto results = make_tl_object<td_api::languagePack>();
    results->languages_.reserve(languages.size());
    for (auto &language : languages) {
      results->languages_.push_back(
          make_tl_object<td_api::languageInfo>(language->lang_code_, language->name_, language->native_name_));
    }
    promise.set_value(std::move(results));
  });
  send_with_promise(G()->net_query_creator().create(create_storer(telegram_api::langpack_getLanguages())),
                    std::move(request_promise));
}

void LanguagePackManager::get_language_pack_strings(string language_code, vector<string> keys,
                                                    Promise<td_api::object_ptr<td_api::languagePackStrings>> promise) {
  Language *language = get_language(database_, language_pack_, language_code);
  if (language_has_strings(language, keys)) {
    return promise.set_value(get_language_pack_strings_object(language, keys));
  }

  if (keys.empty()) {
    auto request_promise =
        PromiseCreator::lambda([actor_id = actor_id(this), language_pack = language_pack_, language_code,
                                promise = std::move(promise)](Result<NetQueryPtr> r_query) mutable {
          auto r_result = fetch_result<telegram_api::langpack_getLangPack>(std::move(r_query));
          if (r_result.is_error()) {
            return promise.set_error(r_result.move_as_error());
          }

          auto result = r_result.move_as_ok();
          LOG(INFO) << "Receive language pack for language " << result->lang_code_ << " from version "
                    << result->from_version_ << " with version " << result->version_ << " of size "
                    << result->strings_.size();
          LOG_IF(ERROR, result->lang_code_ != language_code)
              << "Receive strings for " << result->lang_code_ << " instead of " << language_code;
          LOG_IF(ERROR, result->from_version_ != 0) << "Receive lang pack from version " << result->from_version_;
          send_closure(actor_id, &LanguagePackManager::on_get_language_pack_strings, std::move(language_pack),
                       std::move(language_code), result->version_, false, vector<string>(), std::move(result->strings_),
                       std::move(promise));
        });
    send_with_promise(G()->net_query_creator().create(create_storer(telegram_api::langpack_getLangPack(language_code))),
                      std::move(request_promise));
  } else {
    auto request_promise =
        PromiseCreator::lambda([actor_id = actor_id(this), language_pack = language_pack_, language_code, keys,
                                promise = std::move(promise)](Result<NetQueryPtr> r_query) mutable {
          auto r_result = fetch_result<telegram_api::langpack_getStrings>(std::move(r_query));
          if (r_result.is_error()) {
            return promise.set_error(r_result.move_as_error());
          }

          send_closure(actor_id, &LanguagePackManager::on_get_language_pack_strings, std::move(language_pack),
                       std::move(language_code), -1, false, std::move(keys), r_result.move_as_ok(), std::move(promise));
        });
    send_with_promise(G()->net_query_creator().create(
                          create_storer(telegram_api::langpack_getStrings(language_code, std::move(keys)))),
                      std::move(request_promise));
  }
}

bool LanguagePackManager::is_valid_key(Slice key) {
  for (auto c : key) {
    if (!is_alnum(c) && c != '_' && c != '.' && c != '-') {
      return false;
    }
  }
  return !key.empty();
}

void LanguagePackManager::add_strings_to_database(Language *language, int32 new_version,
                                                  vector<std::pair<string, string>> strings) {
  if (new_version == -1 && strings.empty()) {
    return;
  }

  std::lock_guard<std::mutex> lock(database_->mutex_);
  auto kv = &language->kv_;
  auto old_version = load_database_language_version(kv);
  if (old_version >= new_version && (old_version != -1 || new_version != -1)) {
    return;
  }

  kv->begin_transaction();
  for (auto str : strings) {
    if (!is_valid_key(str.first)) {
      LOG(ERROR) << "Have invalid key \"" << str.first << '"';
      continue;
    }

    kv->set(str.first, str.second);
  }
  if (old_version != new_version) {
    kv->set("!version", to_string(new_version));
  }
  kv->commit_transaction();
}

void LanguagePackManager::on_get_language_pack_strings(
    string language_pack, string language_code, int32 version, bool is_diff, vector<string> keys,
    vector<tl_object_ptr<telegram_api::LangPackString>> results,
    Promise<td_api::object_ptr<td_api::languagePackStrings>> promise) {
  Language *language = get_language(database_, language_pack, language_code);
  bool is_version_changed = false;
  int32 new_database_version = -1;
  vector<std::pair<string, string>> database_strings;
  if (language == nullptr || (language->version_ < version || !keys.empty())) {
    if (language == nullptr) {
      language = add_language(database_, language_pack, language_code);
      CHECK(language != nullptr);
    }
    std::lock_guard<std::mutex> lock(language->mutex_);
    if (language->version_ < version || !keys.empty()) {
      vector<td_api::object_ptr<td_api::LanguagePackString>> strings;
      if (language->version_ < version && !(is_diff && language->version_ == -1)) {
        LOG(INFO) << "Set language " << language_code << " version to " << version;
        language->version_ = version;
        new_database_version = version;
        is_version_changed = true;
      }

      for (auto &result : results) {
        CHECK(result != nullptr);
        switch (result->get_id()) {
          case telegram_api::langPackString::ID: {
            auto str = static_cast<telegram_api::langPackString *>(result.get());
            auto it = language->ordinary_strings_.find(str->key_);
            if (it == language->ordinary_strings_.end()) {
              it = language->ordinary_strings_.emplace(str->key_, std::move(str->value_)).first;
            } else {
              it->second = std::move(str->value_);
            }
            language->pluralized_strings_.erase(str->key_);
            language->deleted_strings_.erase(str->key_);
            if (is_diff) {
              strings.push_back(get_language_pack_string_object(*it));
            }
            database_strings.emplace_back(str->key_, PSTRING() << '1' << it->second);
            break;
          }
          case telegram_api::langPackStringPluralized::ID: {
            auto str = static_cast<const telegram_api::langPackStringPluralized *>(result.get());
            PluralizedString value{std::move(str->zero_value_), std::move(str->one_value_),
                                   std::move(str->two_value_),  std::move(str->few_value_),
                                   std::move(str->many_value_), std::move(str->other_value_)};
            auto it = language->pluralized_strings_.find(str->key_);
            if (it == language->pluralized_strings_.end()) {
              it = language->pluralized_strings_.emplace(str->key_, std::move(value)).first;
            } else {
              it->second = std::move(value);
            }
            language->ordinary_strings_.erase(str->key_);
            language->deleted_strings_.erase(str->key_);
            if (is_diff) {
              strings.push_back(get_language_pack_string_object(*it));
            }
            database_strings.emplace_back(
                str->key_, PSTRING() << '2' << it->second.zero_value_ << '\x00' << it->second.one_value_ << '\x00'
                                     << it->second.two_value_ << '\x00' << it->second.few_value_ << '\x00'
                                     << it->second.many_value_ << '\x00' << it->second.other_value_);
            break;
          }
          case telegram_api::langPackStringDeleted::ID: {
            auto str = static_cast<const telegram_api::langPackStringDeleted *>(result.get());
            language->ordinary_strings_.erase(str->key_);
            language->pluralized_strings_.erase(str->key_);
            language->deleted_strings_.insert(std::move(str->key_));
            if (is_diff) {
              strings.push_back(get_language_pack_string_object(str->key_));
            }
            database_strings.emplace_back(str->key_, "");
            break;
          }
          default:
            UNREACHABLE();
            break;
        }
      }
      for (auto &key : keys) {
        if (!language_has_string_unsafe(language, key)) {
          LOG(ERROR) << "Doesn't receive key " << key << " from server";
          language->deleted_strings_.insert(key);
          if (is_diff) {
            strings.push_back(get_language_pack_string_object(key));
          }
          database_strings.emplace_back(key, "");
        }
      }

      if (is_diff) {
        // do we need to send update to all client instances?
        send_closure(G()->td(), &Td::send_update,
                     td_api::make_object<td_api::updateLanguagePack>(language_pack, language_code, std::move(strings)));
      }
    }
  }

  add_strings_to_database(language, new_database_version, std::move(database_strings));

  if (is_diff) {
    CHECK(language != nullptr);
    std::lock_guard<std::mutex> lock(language->mutex_);
    if (language->has_get_difference_query_) {
      language->has_get_difference_query_ = false;
      is_version_changed = true;
    }
  }
  if (is_version_changed && language_pack == language_pack_ && language_code == language_code_) {
    send_closure_later(actor_id(this), &LanguagePackManager::on_language_pack_version_changed, -1);
  }

  if (promise) {
    promise.set_value(get_language_pack_strings_object(language, keys));
  }
}

void LanguagePackManager::on_failed_get_difference(string language_pack, string language_code) {
  Language *language = get_language(database_, language_pack, language_code);
  CHECK(language != nullptr);
  std::lock_guard<std::mutex> lock(language->mutex_);
  if (language->has_get_difference_query_) {
    language->has_get_difference_query_ = false;
    if (language_pack == language_pack_ && language_code == language_code_) {
      send_closure_later(actor_id(this), &LanguagePackManager::on_language_pack_version_changed, -1);
    }
  }
}

void LanguagePackManager::on_result(NetQueryPtr query) {
  auto token = get_link_token();
  container_.extract(token).set_value(std::move(query));
}

void LanguagePackManager::send_with_promise(NetQueryPtr query, Promise<NetQueryPtr> promise) {
  auto id = container_.create(std::move(promise));
  G()->net_query_dispatcher().dispatch_with_callback(std::move(query), actor_shared(this, id));
}

void LanguagePackManager::hangup() {
  container_.for_each(
      [](auto id, Promise<NetQueryPtr> &promise) { promise.set_error(Status::Error(500, "Request aborted")); });
  stop();
}

int32 LanguagePackManager::manager_count_ = 0;
std::mutex LanguagePackManager::language_database_mutex_;
std::unordered_map<string, std::unique_ptr<LanguagePackManager::LanguageDatabase>>
    LanguagePackManager::language_databases_;

}  // namespace td
