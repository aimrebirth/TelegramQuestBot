// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2018-2024 lzwdgc

#include "curl_http_client.h"

#include <fmt/format.h>
#include <lua.hpp>
#include <primitives/sw/main.h>
#include <primitives/sw/settings.h>
#include <primitives/yaml.h>
#include <primitives/http.h>
#include <tgbot/bot.h>

#include <format>
#include <iostream>
#include <random>
#include <unordered_map>
#include <variant>

struct user
{
    int32_t id;
    String screen;
    String language{"ru"};
    lua_State *L = nullptr;
    std::unordered_map<String, String> variable_types;

    ~user()
    {
        if (L)
            lua_close(L);
    }
};

struct TgQuest
{
    tgbot::bot<curl_http_client> &bot;
    yaml quests;
    yaml screens;
    String initial_screen;
    std::unordered_map<tgbot::Integer, user> users;
    mutable std::mt19937_64 rng;

    TgQuest(auto &bot, const yaml &quests)
        : bot(bot)
        , quests(quests)
        , screens(quests["screens"])
        , rng(std::random_device()())
    {
        initial_screen = quests["initial_screen"].as<String>();
    }

    String getText(user &u, const yaml &v) const
    {
        if (!v["text"].IsDefined())
            return "error: missing text";
        if (v["text"].IsScalar())
            return v["text"].template as<String>();
        if (v["text"].IsMap())
        {
            if (!v["text"][u.language].IsDefined())
                return "error: no translation for language '" + u.language + "' on screen '" + u.screen + "'";
            auto s = v["text"][u.language].template as<String>();
            if (v["text"]["prefix"].IsDefined())
                s = v["text"]["prefix"].template as<String>() + " " + s;
            if (v["text"]["suffix"].IsDefined())
                s = s + v["text"]["suffix"].template as<String>();
            return s;
        }
        return "error: missing text";
    }

    String findScreenByMessage(user &u, const String &msg) const
    {
        auto find = [this, &msg, &u](const auto &row) -> String
        {
            for (const auto &v : row)
            {
                auto b = std::make_shared<tgbot::KeyboardButton>();
                if (msg == getText(u, v.second))
                {
                    if (v.second["language"].IsDefined())
                    {
                        u.language = v.second["language"].template as<String>();
                    }
                    if (v.second["exits"].IsDefined())
                    {
                        if (v.second["exits"].IsSequence())
                        {
                            auto seq = get_sequence<String>(v.second["exits"]);
                            std::uniform_int_distribution<> d(0, seq.size() - 1);
                            return seq[d(rng)];
                        }
                    }
                    return v.first.template as<String>();
                }
            }
            return {};
        };

        if (screens[u.screen]["buttons"]["rows"].IsDefined())
        {
            for (const auto &row : screens[u.screen]["buttons"]["rows"])
            {
                if (auto s = find(row.second); !s.empty())
                    return s;
            }
        }
        else if (screens[u.screen]["buttons"].IsDefined())
        {
            if (auto s = find(screens[u.screen]["buttons"]); !s.empty())
                return s;
        }
        else
            SW_UNIMPLEMENTED;
        return {};
    }

    auto make_keyboard(user &u, const yaml &buttons) const
    {
        decltype(tgbot::ReplyKeyboardMarkup::keyboard) keyboard;

        auto print_row = [this, &keyboard, &u](const auto &row)
        {
            decltype(keyboard)::value_type btns;
            for (const auto &v : row)
            {
                auto b = std::make_unique<tgbot::KeyboardButton>();
                b->text = getText(u, v.second);
                btns.push_back(std::move(b));
            }
            return btns;
        };

        if (buttons["buttons"]["rows"].IsDefined())
        {
            for (const auto &row : buttons["buttons"]["rows"])
                keyboard.push_back(print_row(row.second));
        }
        else if (buttons["buttons"].IsDefined())
        {
            keyboard.push_back(print_row(buttons["buttons"]));
        }
        else
            SW_UNIMPLEMENTED;
        return keyboard;
    }

    void show_screen(user &u) const
    {
        if (screens[u.screen]["quest"].IsDefined() &&
            screens[u.screen]["quest"].as<bool>())
        {
            if (u.L)
                lua_close(u.L);
            u.L = luaL_newstate();
            luaopen_base(u.L);
        }

        if (screens[u.screen]["variables"].IsDefined())
        {
            for (const auto &kv : screens[u.screen]["variables"])
                u.variable_types[kv.first.as<String>()] = kv.second.as<String>();
        }

        auto text = getText(u, screens[u.screen]);
        if (auto err = execute_script(u, screens[u.screen]); !err.empty())
            text = err;

        if (u.L)
        {
            std::unordered_set<String> vars;

            // enumerate globals
            lua_pushglobaltable(u.L);       // Get global table
            lua_pushnil(u.L);               // put a nil key on stack
            while (lua_next(u.L, -2) != 0)  // key(-1) is replaced by the next key(-1) in table(-2)
            {
                auto name = lua_tostring(u.L, -2);  // Get key(-2) name
                vars.insert(name);
                lua_pop(u.L, 1);               // remove value(-1), now key on top at(-1)
            }
            lua_pop(u.L, 1);                 // remove global table(-1)

            for (auto &name : vars)
            {
                auto i = u.variable_types.find(name);
                if (i != u.variable_types.end())
                {
                    auto &t = i->second;
                    if (t == "int")
                        text = fmt::format(fmt::runtime(text), fmt::arg(name.c_str(), (int)getIntField(u.L, name.c_str())));
                    else if (t == "string")
                        text = fmt::format(fmt::runtime(text), fmt::arg(name.c_str(), getStringField(u.L, name.c_str())));
                    else if (t == "float")
                        text = fmt::format(fmt::runtime(text), fmt::arg(name.c_str(), getIntField(u.L, name.c_str())));
                }
            }
        }

        tgbot::ReplyKeyboardMarkup mk;
        //mk->oneTimeKeyboard = true;
        mk.resize_keyboard = true;
        mk.keyboard = make_keyboard(u, screens[u.screen]);

        tgbot::sendMessageRequest req;
        req.chat_id = u.id;
        req.text = text;
        req.parse_mode = tgbot::ParseMode::HTML;
        req.reply_markup = std::move(mk);
        bot.api().sendMessage(req);
    }

    String execute_script(user &u, const yaml &v) const
    {
        if (v["script"].IsDefined() && u.L)
        {
            // load file
            if (luaL_loadstring(u.L, screens[u.screen]["script"].as<String>().c_str()))
            {
                return "Error during lua script loading";
            }
            // execute global statements
            else if (lua_pcall(u.L, 0, 0, 0))
            {
                return "Error during lua script execution";
            }
        }
        return {};
    }

    static double getIntField(lua_State* L, const char* key)
    {
        lua_pushstring(L, key);
        lua_gettable(L, -2);  // get table[key]

        auto result = lua_tonumber(L, -1);
        lua_pop(L, 1);  // remove number from stack
        return result;
    }

    static std::string getStringField(lua_State* L, const char* key)
    {
        lua_pushstring(L, key);
        lua_gettable(L, -2);  // get table[key]

        std::string result = lua_tostring(L, -1);
        lua_pop(L, 1);  // remove string from stack
        return result;
    }

    //
    std::string botname;
    std::string botvisiblename;

    static const int default_update_limit = 100;
    static const int default_update_timeout = 10;
    int net_delay_on_error = 1;

    tgbot::Integer process_updates(tgbot::Integer offset = 0, tgbot::Integer limit = default_update_limit,
                                   tgbot::Integer timeout = default_update_timeout,
                                   const tgbot::Optional<tgbot::Vector<String>> &allowed_updates = {}) {
        // update timeout here for getUpdates()
        ((curl_http_client &)bot.http_client()).set_timeout(timeout);

        auto updates = bot.api().getUpdates(offset, limit, timeout, allowed_updates);
        for (const auto &item : updates) {
            // if updates come unsorted, we must check this
            if (item.update_id >= offset)
                offset = item.update_id + 1;
            process_update(item);
        }
        return offset;
    }
    void process_update(const tgbot::Update &update) {
        try {
            handle_update(update);
        } catch (std::exception &e) {
            printf("error: %s\n", e.what());

            std::this_thread::sleep_for(std::chrono::seconds(net_delay_on_error));
            if (net_delay_on_error < 30)
                net_delay_on_error *= 2;
        }
    }
    void long_poll(tgbot::Integer limit = default_update_limit, tgbot::Integer timeout = default_update_timeout,
                   const tgbot::Optional<tgbot::Vector<String>> &allowed_updates = {}) {
        tgbot::Integer offset = 0;
        while (1) {
            try {
                offset = process_updates(offset, limit, timeout, allowed_updates);
            } catch (std::exception &e) {
                printf("error: %s\n", e.what());
                ++offset; // we skip erroneous message
            }
        }
    }
    void handle_update(const tgbot::Update &update) {
        if (update.message) {
            handle_message(*update.message);
        }
    }
    void handle_message(const tgbot::Message &message) {
        if (!message.text) {
            return;
        }
        if (message.text->starts_with("/")) {
            if (*message.text == "/start") {
                auto &u = users[message.from->id];
                u.id = message.from->id;
                u.screen = initial_screen;
                show_screen(u);
            }
            return;
        }

        auto &u = users[message.from->id];
        u.id = message.from->id;
        if (u.screen.empty()) {
            u.screen = initial_screen;
            show_screen(u);
            return;
        }
        if (auto s = findScreenByMessage(u, *message.text); !s.empty()) {
            if (screens[s].IsDefined())
                u.screen = s;
            show_screen(u);
        }
    }
};

int main(int argc, char *argv[]) {
    primitives::http::setupSafeTls();

    sw::setting<std::string> proxy_host("proxy_host");
    sw::setting<std::string> proxy_user("proxy_user");

    const auto root = YAML::LoadFile(sw::getSettings(sw::SettingsType::Local)["quests_file"].as<String>());

    curl_http_client client;
    auto bot = std::make_unique<tgbot::bot<curl_http_client>>(sw::getSettings(sw::SettingsType::Local)["bot_token"].as<String>(), client);

    // setup connection
    auto curl = client.curl_settings();

    if (!proxy_host.getValue().empty())
    {
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy_host.getValue().c_str());
        curl_easy_setopt(curl, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
        if (proxy_host.getValue().find("socks5") == 0)
        {
            curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);
            curl_easy_setopt(curl, CURLOPT_SOCKS5_AUTH, CURLAUTH_BASIC);
            curl_easy_setopt(curl, CURLOPT_PROXYAUTH, CURLAUTH_BASIC);
        }
        if (!proxy_user.getValue().empty())
            curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD, proxy_user.getValue().c_str());
    }

    TgQuest q(*bot, root);
    while (1)
    {
        try
        {
            auto me = bot->api().getMe();
            if (me.username)
                printf("Bot username: %s\n", me.username->c_str());
            q.long_poll();
        }
        catch (std::exception &e)
        {
            printf("error: %s\n", e.what());
        }
    }

    return 0;
}
