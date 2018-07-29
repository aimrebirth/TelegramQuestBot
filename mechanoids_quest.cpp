/*
 * mechanoids quest
 * Copyright (C) 2018 lzwdgc
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
local_settings:
    use_shared_libs: false
    builds:
        default:
            x: x
        gcc:
            generator: Ninja

c++: 17
dependencies:
    - pvt.cppan.demo.reo7sp.tgbot: master
    - pvt.cppan.demo.lua: 5
    - pvt.cppan.demo.fmt: 5
    - pvt.egorpugin.primitives.sw.main: master
options:
    any:
        link_options:
            gnu:
                private:
                    - -static-libstdc++
                    - -static-libgcc
*/

#include <fmt/format.h>
#include <lua.hpp>
#include <primitives/sw/main.h>
#include <primitives/sw/settings.h>
#include <primitives/yaml.h>
#include <tgbot/tgbot.h>

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
    TgBot::Bot &bot;
    yaml quests;
    yaml screens;
    String initial_screen;
    std::unordered_map<int32_t, user> users;
    mutable std::mt19937_64 rng;

    TgQuest(TgBot::Bot &bot, const yaml &quests)
        : bot(bot)
        , quests(quests)
        , screens(quests["screens"])
        , rng(std::random_device()())
    {
        initial_screen = quests["initial_screen"].as<String>();

        bot.getEvents().onCommand("start", [&](TgBot::Message::Ptr message)
        {
            auto &u = users[message->from->id];
            u.id = message->from->id;
            u.screen = initial_screen;
            show_screen(u);
        });

        bot.getEvents().onAnyMessage([&](TgBot::Message::Ptr message)
        {
            if (StringTools::startsWith(message->text, "/"))
                return;

            auto &u = users[message->from->id];
            u.id = message->from->id;
            if (u.screen.empty())
            {
                u.screen = initial_screen;
                show_screen(u);
                return;
            }
            if (auto s = findScreenByMessage(u, message->text); !s.empty())
            {
                if (screens[s].IsDefined())
                    u.screen = s;
                show_screen(u);
            }
        });
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
                auto b = std::make_shared<TgBot::KeyboardButton>();
                if (msg == getText(u, v.second))
                {
                    if (v.second["language"].IsDefined())
                    {
                        u.language = v.second["language"].as<String>();
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
            assert(false);
        return {};
    }

    std::vector<std::vector<TgBot::KeyboardButton::Ptr>> make_keyboard(user &u, const yaml &buttons) const
    {
        std::vector<std::vector<TgBot::KeyboardButton::Ptr>> keyboard;

        auto print_row = [this, &keyboard, &u](const auto &row)
        {
            std::vector<TgBot::KeyboardButton::Ptr> btns;
            for (const auto &v : row)
            {
                auto b = std::make_shared<TgBot::KeyboardButton>();
                b->text = getText(u, v.second);
                btns.push_back(b);
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
            assert(false);
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
                        text = fmt::format(text, fmt::arg(name, (int)getIntField(u.L, name.c_str())));
                    else if (t == "string")
                        text = fmt::format(text, fmt::arg(name, getStringField(u.L, name.c_str())));
                    else if (t == "float")
                        text = fmt::format(text, fmt::arg(name, getIntField(u.L, name.c_str())));
                }
            }
        }

        auto mk = std::make_shared<TgBot::ReplyKeyboardMarkup>();
        //mk->oneTimeKeyboard = true;
        mk->resizeKeyboard = true;
        mk->keyboard = make_keyboard(u, screens[u.screen]);
        bot.getApi().sendMessage(u.id, text, false, 0, mk, "HTML");
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
};

int main(int argc, char **argv)
{
    sw::setting<bool> use_curl("use_curl");
    sw::setting<std::string> proxy_host("proxy_host");
    sw::setting<std::string> proxy_user("proxy_user");

    // setup connection
    TgBot::CurlHttpClient curl;

    curl_easy_setopt(curl.curlSettings, CURLOPT_SSL_VERIFYPEER, 0);
    //curl_easy_setopt(curl.curlSettings, CURLOPT_SSL_VERIFYHOST, 2);

    if (!proxy_host.getValue().empty())
    {
        use_curl = true;
        curl_easy_setopt(curl.curlSettings, CURLOPT_PROXY, proxy_host.getValue().c_str());
        curl_easy_setopt(curl.curlSettings, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
        if (proxy_host.getValue().find("socks5") == 0)
        {
            curl_easy_setopt(curl.curlSettings, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);
            curl_easy_setopt(curl.curlSettings, CURLOPT_SOCKS5_AUTH, CURLAUTH_BASIC);
        }
        if (!proxy_user.getValue().empty())
            curl_easy_setopt(curl.curlSettings, CURLOPT_PROXYUSERPWD, proxy_user.getValue().c_str());
    }

    const auto root = YAML::LoadFile(getSettings().getLocalSettings()["quests_file"].as<String>());
    std::unique_ptr<TgBot::Bot> bot;
    if (use_curl)
        bot = std::make_unique<TgBot::Bot>(getSettings().getLocalSettings()["bot_token"].as<String>(), curl);
    else
        bot = std::make_unique<TgBot::Bot>(getSettings().getLocalSettings()["bot_token"].as<String>());
    TgQuest q(*bot, root);

    while (1)
    {
        try
        {
            printf("Bot username: %s\n", bot->getApi().getMe()->username.c_str());
            TgBot::TgLongPoll longPoll(*bot);
            while (1)
                longPoll.start();
        }
        catch (TgBot::TgException &e)
        {
            printf("tg error: %s\n", e.what());
        }
        catch (std::exception &e)
        {
            printf("error: %s\n", e.what());
        }
    }

    return 0;
}
