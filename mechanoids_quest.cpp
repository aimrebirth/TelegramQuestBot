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
    - pvt.egorpugin.primitives.yaml: master
    - pvt.cppan.demo.lua: 5
    - pvt.cppan.demo.fmt: 5
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
    std::mt19937_64 rng;

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

    String findScreenByMessage(user &u, const String &msg)
    {
        auto find = [this, &msg](const auto &row) -> String
        {
            for (const auto &v : row)
            {
                auto b = std::make_shared<TgBot::KeyboardButton>();
                if (msg == v.second["text"].template as<String>())
                {
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

    std::vector<std::vector<TgBot::KeyboardButton::Ptr>> make_keyboard(const yaml &buttons)
    {
        std::vector<std::vector<TgBot::KeyboardButton::Ptr>> keyboard;

        auto print_row = [&keyboard](const auto &row)
        {
            std::vector<TgBot::KeyboardButton::Ptr> btns;
            for (const auto &v : row)
            {
                auto b = std::make_shared<TgBot::KeyboardButton>();
                b->text = v.second["text"].template as<String>();
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

    void show_screen(user &u)
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

        auto text = screens[u.screen]["text"].as<String>();
        if (screens[u.screen]["script"].IsDefined() && u.L)
        {
            // load file
            if (luaL_loadstring(u.L, screens[u.screen]["script"].as<String>().c_str()))
            {
                text = "Error during lua script loading";
            }
            // execute global statements
            else if (lua_pcall(u.L, 0, 0, 0))
            {
                text = "Error during lua script execution";
            }
        }

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
        mk->keyboard = make_keyboard(screens[u.screen]);
        bot.getApi().sendMessage(u.id, text, false, 0, mk, "Markdown");
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
    if (argc != 3)
    {
        std::cerr << "bot api_key quests.yml\n";
        return 1;
    }

    const auto root = YAML::LoadFile(argv[2]);
    TgBot::Bot bot(argv[1]);
    TgQuest q(bot, root);

    while (1)
    {
        try
        {
            printf("Bot username: %s\n", bot.getApi().getMe()->username.c_str());
            TgBot::TgLongPoll longPoll(bot);
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
