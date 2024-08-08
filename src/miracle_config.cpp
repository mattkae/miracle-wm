/**
Copyright (C) 2024  Matthew Kosarek

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
**/

#include <glm/fwd.hpp>
#define MIR_LOG_COMPONENT "miracle_config"

#include "miracle_config.h"
#include "yaml-cpp/node/node.h"
#include "yaml-cpp/yaml.h"
#include <cstdlib>
#include <fstream>
#include <glib-2.0/glib.h>
#include <libevdev-1.0/libevdev/libevdev.h>
#include <libnotify/notify.h>
#include <mir/log.h>
#include <miral/runner.h>
#include <sys/inotify.h>

using namespace miracle;

namespace
{
int program_exists(std::string const& name)
{
    std::stringstream out;
    out << "command -v " << name << " > /dev/null 2>&1";
    return !system(out.str().c_str());
}

glm::vec4 parse_color(YAML::Node const& node)
{
    const float MAX_COLOR_VALUE = 255;
    float r, g, b, a;
    if (node.IsMap())
    {
        // Parse as (r, g, b, a) object
        if (!node["r"])
        {
        }
        r = node["r"].as<float>() / MAX_COLOR_VALUE;
        g = node["g"].as<float>() / MAX_COLOR_VALUE;
        b = node["b"].as<float>() / MAX_COLOR_VALUE;
        a = node["a"].as<float>() / MAX_COLOR_VALUE;
    }
    else if (node.IsSequence())
    {
        // Parse as [r, g, b, a] array
        r = node[0].as<float>() / MAX_COLOR_VALUE;
        g = node[1].as<float>() / MAX_COLOR_VALUE;
        b = node[2].as<float>() / MAX_COLOR_VALUE;
        a = node[3].as<float>() / MAX_COLOR_VALUE;
    }
    else
    {
        // Parse as hex color
        auto value = node.as<std::string>();
        unsigned int i = std::stoul(value, nullptr, 16);
        r = static_cast<float>(((i >> 24) & 0xFF)) / MAX_COLOR_VALUE;
        g = static_cast<float>(((i >> 16) & 0xFF)) / MAX_COLOR_VALUE;
        b = static_cast<float>(((i >> 8) & 0xFF)) / MAX_COLOR_VALUE;
        a = static_cast<float>((i & 0xFF)) / MAX_COLOR_VALUE;
    }

    r = std::clamp(r, 0.f, 1.f);
    g = std::clamp(g, 0.f, 1.f);
    b = std::clamp(b, 0.f, 1.f);
    a = std::clamp(a, 0.f, 1.f);

    return { r, g, b, a };
}

std::string wrap_command(std::string const& command)
{
    return command;
}

template <typename T>
bool try_parse_value(YAML::Node const& root, const char* key, T& value)
{
    if (!root[key])
        return false;

    auto const& node = root[key];
    try
    {
        value = node.as<T>();
    }
    catch (YAML::BadConversion const& e)
    {
        mir::log_error("(L%d) Unable to parse '%s: %s", node.Mark().line, key, e.msg.c_str());
        return false;
    }
    return true;
}

template <typename T>
T try_parse_enum(YAML::Node const& root, const char* key, std::function<T(std::string const&)> const& parse, T invalid)
{
    T result = invalid;
    if (!root[key])
        return invalid;

    auto const& node = root[key];
    try
    {
        result = parse(node.as<std::string>());
        if (result == invalid)
        {
            mir::log_error("(L%d) '%s' is invalid", node.Mark().line, key);
            return invalid;
        }
    }
    catch (YAML::BadConversion const& e)
    {
        mir::log_error("(L%d) Unable to parse '%s: %s", node.Mark().line, key, e.msg.c_str());
        return invalid;
    }
    return result;
}
}

FilesystemConfiguration::FilesystemConfiguration(miral::MirRunner& runner) :
    runner { runner }
{
    std::stringstream config_path_stream;
    config_path_stream << g_get_user_config_dir();
    config_path_stream << "/miracle-wm.yaml";
    config_path = config_path_stream.str();

    mir::log_info("Configuration file path is: %s", config_path.c_str());

    {
        std::fstream file(config_path, std::ios::out | std::ios::in | std::ios::app);
    }

    _load();
    _watch(runner);
}

FilesystemConfiguration::FilesystemConfiguration(miral::MirRunner& runner, std::string const& path) :
    runner { runner },
    config_path { path }
{
    {
        std::fstream file(config_path, std::ios::out | std::ios::in | std::ios::app);
    }

    mir::log_info("Configuration file path is: %s", config_path.c_str());
    _load();
    _watch(runner);
}

void FilesystemConfiguration::_load()
{
    std::lock_guard<std::mutex> lock(mutex);

    // Reset all
    primary_modifier = mir_input_event_modifier_meta;
    custom_key_commands = {};
    inner_gaps_x = 10;
    inner_gaps_y = 10;
    outer_gaps_x = 10;
    outer_gaps_y = 10;
    startup_apps = {};
    terminal = wrap_command("miracle-wm-sensible-terminal");
    desired_terminal = "";
    resize_jump = 50;
    border_config = { 0, glm::vec4(0), glm::vec4(0) };

    // Load the new configuration
    mir::log_info("Configuration is loading...");
    YAML::Node config = YAML::LoadFile(config_path);
    if (config["action_key"])
    {
        auto const stringified_action_key = config["action_key"].as<std::string>();
        auto modifier = parse_modifier(stringified_action_key);
        if (modifier == mir_input_event_modifier_none)
            mir::log_error("action_key: invalid action key: %s", stringified_action_key.c_str());
        else
            primary_modifier = parse_modifier(stringified_action_key);
    }

    if (config["default_action_overrides"])
    {
        auto const default_action_overrides = config["default_action_overrides"];
        if (!default_action_overrides.IsSequence())
        {
            mir::log_error("default_action_overrides: value must be an array");
            return;
        }

        for (auto i = 0; i < default_action_overrides.size(); i++)
        {
            auto sub_node = default_action_overrides[i];
            if (!sub_node["name"])
            {
                mir::log_error("default_action_overrides: missing name");
                continue;
            }

            if (!sub_node["action"])
            {
                mir::log_error("default_action_overrides: missing action");
                continue;
            }

            if (!sub_node["modifiers"])
            {
                mir::log_error("default_action_overrides: missing modifiers");
                continue;
            }

            if (!sub_node["key"])
            {
                mir::log_error("default_action_overrides: missing key");
                continue;
            }

            std::string name;
            std::string action;
            std::string key;
            YAML::Node modifiers_node;
            try
            {
                name = sub_node["name"].as<std::string>();
                action = sub_node["action"].as<std::string>();
                key = sub_node["key"].as<std::string>();
                modifiers_node = sub_node["modifiers"];
            }
            catch (YAML::BadConversion const& e)
            {
                mir::log_error("Unable to parse default_action_override[%d]: %s", i, e.msg.c_str());
                continue;
            }

            DefaultKeyCommand key_command;
            if (name == "terminal")
                key_command = DefaultKeyCommand::Terminal;
            else if (name == "request_vertical_layout")
                key_command = DefaultKeyCommand::RequestVertical;
            else if (name == "request_horizontal_layout")
                key_command = DefaultKeyCommand::RequestHorizontal;
            else if (name == "toggle_resize")
                key_command = DefaultKeyCommand::ToggleResize;
            else if (name == "resize_up")
                key_command = DefaultKeyCommand::ResizeUp;
            else if (name == "resize_down")
                key_command = DefaultKeyCommand::ResizeDown;
            else if (name == "resize_left")
                key_command = DefaultKeyCommand::ResizeLeft;
            else if (name == "resize_right")
                key_command = DefaultKeyCommand::ResizeRight;
            else if (name == "move_up")
                key_command = DefaultKeyCommand::MoveUp;
            else if (name == "move_down")
                key_command = DefaultKeyCommand::MoveDown;
            else if (name == "move_left")
                key_command = DefaultKeyCommand::MoveLeft;
            else if (name == "move_right")
                key_command = DefaultKeyCommand::MoveRight;
            else if (name == "select_up")
                key_command = DefaultKeyCommand::SelectUp;
            else if (name == "select_down")
                key_command = DefaultKeyCommand::SelectDown;
            else if (name == "select_left")
                key_command = DefaultKeyCommand::SelectLeft;
            else if (name == "select_right")
                key_command = DefaultKeyCommand::SelectRight;
            else if (name == "quit_active_window")
                key_command = DefaultKeyCommand::QuitActiveWindow;
            else if (name == "quit_compositor")
                key_command = DefaultKeyCommand::QuitCompositor;
            else if (name == "fullscreen")
                key_command = DefaultKeyCommand::Fullscreen;
            else if (name == "select_workspace_1")
                key_command = DefaultKeyCommand::SelectWorkspace1;
            else if (name == "select_workspace_2")
                key_command = DefaultKeyCommand::SelectWorkspace2;
            else if (name == "select_workspace_3")
                key_command = DefaultKeyCommand::SelectWorkspace3;
            else if (name == "select_workspace_4")
                key_command = DefaultKeyCommand::SelectWorkspace4;
            else if (name == "select_workspace_5")
                key_command = DefaultKeyCommand::SelectWorkspace5;
            else if (name == "select_workspace_6")
                key_command = DefaultKeyCommand::SelectWorkspace6;
            else if (name == "select_workspace_7")
                key_command = DefaultKeyCommand::SelectWorkspace7;
            else if (name == "select_workspace_8")
                key_command = DefaultKeyCommand::SelectWorkspace8;
            else if (name == "select_workspace_9")
                key_command = DefaultKeyCommand::SelectWorkspace9;
            else if (name == "select_workspace_0")
                key_command = DefaultKeyCommand::SelectWorkspace0;
            else if (name == "move_to_workspace_1")
                key_command = DefaultKeyCommand::MoveToWorkspace1;
            else if (name == "move_to_workspace_2")
                key_command = DefaultKeyCommand::MoveToWorkspace2;
            else if (name == "move_to_workspace_3")
                key_command = DefaultKeyCommand::MoveToWorkspace3;
            else if (name == "move_to_workspace_4")
                key_command = DefaultKeyCommand::MoveToWorkspace4;
            else if (name == "move_to_workspace_5")
                key_command = DefaultKeyCommand::MoveToWorkspace5;
            else if (name == "move_to_workspace_6")
                key_command = DefaultKeyCommand::MoveToWorkspace6;
            else if (name == "move_to_workspace_7")
                key_command = DefaultKeyCommand::MoveToWorkspace7;
            else if (name == "move_to_workspace_8")
                key_command = DefaultKeyCommand::MoveToWorkspace8;
            else if (name == "move_to_workspace_9")
                key_command = DefaultKeyCommand::MoveToWorkspace9;
            else if (name == "move_to_workspace_0")
                key_command = DefaultKeyCommand::MoveToWorkspace0;
            else if (name == "toggle_floating")
                key_command = DefaultKeyCommand::ToggleFloating;
            else if (name == "toggle_pinned_to_workspace")
                key_command = DefaultKeyCommand::TogglePinnedToWorkspace;
            else
            {
                mir::log_error("default_action_overrides: Unknown key command override: %s", name.c_str());
                continue;
            }

            MirKeyboardAction keyboard_action;
            if (action == "up")
                keyboard_action = MirKeyboardAction::mir_keyboard_action_up;
            else if (action == "down")
                keyboard_action = MirKeyboardAction::mir_keyboard_action_down;
            else if (action == "repeat")
                keyboard_action = MirKeyboardAction::mir_keyboard_action_repeat;
            else if (action == "modifiers")
                keyboard_action = MirKeyboardAction::mir_keyboard_action_modifiers;
            else
            {
                mir::log_error("default_action_overrides: Unknown keyboard action: %s", action.c_str());
                continue;
            }

            auto code = libevdev_event_code_from_name(EV_KEY, key.c_str()); // https://stackoverflow.com/questions/32059363/is-there-a-way-to-get-the-evdev-keycode-from-a-string
            if (code < 0)
            {
                mir::log_error("default_action_overrides: Unknown keyboard code in configuration: %s. See the linux kernel for allowed codes: https://github.com/torvalds/linux/blob/master/include/uapi/linux/input-event-codes.h", key.c_str());
                continue;
            }

            if (!modifiers_node.IsSequence())
            {
                mir::log_error("default_action_overrides: Provided modifiers is not an array");
                continue;
            }

            uint modifiers = 0;
            bool is_invalid = false;
            for (auto j = 0; j < modifiers_node.size(); j++)
            {
                try
                {
                    auto modifier = modifiers_node[j].as<std::string>();
                    modifiers = modifiers | parse_modifier(modifier);
                }
                catch (YAML::BadConversion const& e)
                {
                    mir::log_error("Unable to parse modifier for default_action_overrides[%d]: %s", i, e.msg.c_str());
                    is_invalid = true;
                    break;
                }
            }

            if (is_invalid)
                continue;

            key_commands[key_command].push_back({ keyboard_action,
                modifiers,
                code });
        }
    }

    // Fill in default actions if the list is zero for any one of them
    const KeyCommand default_key_commands[DefaultKeyCommand::MAX] = {
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_ENTER },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_V     },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_H     },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_R     },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_UP    },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_DOWN  },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_LEFT  },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_RIGHT },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_UP    },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_DOWN  },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_LEFT  },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_RIGHT },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_UP    },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_DOWN  },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_LEFT  },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_RIGHT },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_Q     },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_E     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_F     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_1     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_2     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_3     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_4     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_5     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_6     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_7     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_8     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_9     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_0     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_1     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_2     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_3     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_4     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_5     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_6     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_7     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_8     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_9     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_0     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_SPACE },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_P     }
    };
    for (int i = 0; i < DefaultKeyCommand::MAX; i++)
    {
        if (key_commands[i].empty())
            key_commands[i].push_back(default_key_commands[i]);
    }

    // Custom actions
    if (config["custom_actions"])
    {
        auto const custom_actions = config["custom_actions"];
        if (!custom_actions.IsSequence())
        {
            mir::log_error("custom_actions: value must be an array");
            return;
        }

        for (auto i = 0; i < custom_actions.size(); i++)
        {
            auto sub_node = custom_actions[i];
            if (!sub_node["command"])
            {
                mir::log_error("custom_actions: missing command");
                continue;
            }

            if (!sub_node["action"])
            {
                mir::log_error("custom_actions: missing action");
                continue;
            }

            if (!sub_node["modifiers"])
            {
                mir::log_error("custom_actions: missing modifiers");
                continue;
            }

            if (!sub_node["key"])
            {
                mir::log_error("custom_actions: missing key");
                continue;
            }

            std::string command;
            std::string action;
            std::string key;
            YAML::Node modifiers_node;
            try
            {
                command = wrap_command(sub_node["command"].as<std::string>());
                action = sub_node["action"].as<std::string>();
                key = sub_node["key"].as<std::string>();
                modifiers_node = sub_node["modifiers"];
            }
            catch (YAML::BadConversion const& e)
            {
                mir::log_error("Unable to parse custom_actions[%d]: %s", i, e.msg.c_str());
                continue;
            }

            // TODO: Copy & paste here
            MirKeyboardAction keyboard_action;
            if (action == "up")
                keyboard_action = MirKeyboardAction::mir_keyboard_action_up;
            else if (action == "down")
                keyboard_action = MirKeyboardAction::mir_keyboard_action_down;
            else if (action == "repeat")
                keyboard_action = MirKeyboardAction::mir_keyboard_action_repeat;
            else if (action == "modifiers")
                keyboard_action = MirKeyboardAction::mir_keyboard_action_modifiers;
            else
            {
                mir::log_error("custom_actions: Unknown keyboard action: %s", action.c_str());
                continue;
            }

            auto code = libevdev_event_code_from_name(EV_KEY,
                key.c_str()); // https://stackoverflow.com/questions/32059363/is-there-a-way-to-get-the-evdev-keycode-from-a-string
            if (code < 0)
            {
                mir::log_error(
                    "custom_actions: Unknown keyboard code in configuration: %s. See the linux kernel for allowed codes: https://github.com/torvalds/linux/blob/master/include/uapi/linux/input-event-codes.h",
                    key.c_str());
                continue;
            }

            if (!modifiers_node.IsSequence())
            {
                mir::log_error("custom_actions: Provided modifiers is not an array");
                continue;
            }

            uint modifiers = 0;
            bool is_invalid = false;
            for (auto j = 0; j < modifiers_node.size(); j++)
            {
                try
                {
                    auto modifier = modifiers_node[j].as<std::string>();
                    modifiers = modifiers | parse_modifier(modifier);
                }
                catch (YAML::BadConversion const& e)
                {
                    mir::log_error("Unable to parse modifier for custom_actions[%d]: %s", i, e.msg.c_str());
                    is_invalid = true;
                    break;
                }
            }

            if (is_invalid)
                continue;

            custom_key_commands.push_back({ keyboard_action,
                modifiers,
                code,
                command });
        }
    }

    // Gap sizes
    if (config["inner_gaps"])
    {
        int new_inner_gaps_x = inner_gaps_x;
        int new_inner_gaps_y = inner_gaps_y;
        try
        {
            if (config["inner_gaps"]["x"])
                new_inner_gaps_x = config["inner_gaps"]["x"].as<int>();
            if (config["inner_gaps"]["y"])
                new_inner_gaps_y = config["inner_gaps"]["y"].as<int>();

            inner_gaps_x = new_inner_gaps_x;
            inner_gaps_y = new_inner_gaps_y;
        }
        catch (YAML::BadConversion const& e)
        {
            mir::log_error("Unable to parse inner_gaps: %s", e.msg.c_str());
        }
    }
    if (config["outer_gaps"])
    {
        try
        {
            int new_outer_gaps_x = outer_gaps_x;
            int new_outer_gaps_y = outer_gaps_y;
            if (config["outer_gaps"]["x"])
                new_outer_gaps_x = config["outer_gaps"]["x"].as<int>();
            if (config["outer_gaps"]["y"])
                new_outer_gaps_y = config["outer_gaps"]["y"].as<int>();

            outer_gaps_x = new_outer_gaps_x;
            outer_gaps_y = new_outer_gaps_y;
        }
        catch (YAML::BadConversion const& e)
        {
            mir::log_error("Unable to parse outer_gaps: %s", e.msg.c_str());
        }
    }

    // Startup Apps
    if (config["startup_apps"])
    {
        if (!config["startup_apps"].IsSequence())
        {
            mir::log_error("startup_apps is not an array");
        }
        else
        {
            for (auto const& node : config["startup_apps"])
            {
                if (!node["command"])
                {
                    mir::log_error("startup_apps: app lacks a command");
                    continue;
                }

                try
                {
                    auto command = wrap_command(node["command"].as<std::string>());
                    bool restart_on_death = false;
                    if (node["restart_on_death"])
                    {
                        restart_on_death = node["restart_on_death"].as<bool>();
                    }

                    startup_apps.push_back({ std::move(command), restart_on_death });
                }
                catch (YAML::BadConversion const& e)
                {
                    mir::log_error("Unable to parse startup_apps: %s", e.msg.c_str());
                }
            }
        }
    }

    // Terminal
    if (config["terminal"])
    {
        try
        {
            terminal = wrap_command(config["terminal"].as<std::string>());
        }
        catch (YAML::BadConversion const& e)
        {
            mir::log_error("Unable to parse terminal: %s", e.msg.c_str());
        }
    }

    if (terminal && !program_exists(terminal.value()))
    {
        desired_terminal = terminal.value();
        terminal.reset();
    }

    // Resizing
    if (config["resize_jump"])
    {
        try
        {
            resize_jump = config["resize_jump"].as<int>();
        }
        catch (YAML::BadConversion const& e)
        {
            mir::log_error("Unable to parse resize_jump: %s", e.msg.c_str());
        }
    }

    // Environment variables
    if (config["environment_variables"])
    {
        if (!config["environment_variables"].IsSequence())
        {
            mir::log_error("environment_variables is not an array");
        }
        else
        {
            for (auto const& node : config["environment_variables"])
            {
                if (!node["key"])
                {
                    mir::log_error("environment_variables: item is missing a 'key'");
                    continue;
                }

                if (!node["value"])
                {
                    mir::log_error("environment_variables: item is missing a 'value'");
                    continue;
                }

                try
                {
                    auto key = node["key"].as<std::string>();
                    auto value = node["value"].as<std::string>();
                    environment_variables.push_back({ key, value });
                }
                catch (YAML::BadConversion const& e)
                {
                    mir::log_error("Unable to parse environment_variable_entry: %s", e.msg.c_str());
                }
            }
        }
    }

    if (config["border"])
    {
        try
        {
            auto border = config["border"];
            auto size = border["size"].as<int>();
            auto color = parse_color(border["color"]);
            auto focus_color = parse_color(border["focus_color"]);
            border_config = { size, focus_color, color };
        }
        catch (YAML::BadConversion const& e)
        {
            mir::log_error("Unable to parse border: %s", e.msg.c_str());
        }
    }

    if (config["workspaces"])
    {
        try
        {
            auto const& workspaces = config["workspaces"];
            if (!workspaces.IsSequence())
            {
                mir::log_error("workspaces: expected sequence: L%d:%d", workspaces.Mark().line, workspaces.Mark().column);
            }
            else
            {
                for (auto const& workspace : workspaces)
                {
                    auto num = workspace["number"].as<int>();
                    auto type = container_type_from_string(workspace["layout"].as<std::string>());
                    if (type != ContainerType::leaf && type != ContainerType::floating_window)
                    {
                        mir::log_error("layout should be 'tiled' or 'floating': L%d:%d", workspace["layout"].Mark().line, workspace["layout"].Mark().column);
                        continue;
                    }

                    workspace_configs.push_back({ num, type });
                }
            }
        }
        catch (YAML::BadConversion const& e)
        {
            mir::log_error("workspaces: unable to parse: %s, L%d:%d", e.msg.c_str(), e.mark.line, e.mark.column);
        }
    }

    read_animation_definitions(config);
}

void FilesystemConfiguration::read_animation_definitions(YAML::Node const& root)
{
    std::array<AnimationDefinition, (int)AnimateableEvent::max> parsed({
        {
         AnimationType::grow,
         EaseFunction::ease_in_out_back,
         0.25f,
         },
        {
         AnimationType::slide,
         EaseFunction::ease_in_out_back,
         0.25f,
         },
        {
         AnimationType::shrink,
         EaseFunction::ease_out_back,
         0.25f,
         },
        { AnimationType::slide,
         EaseFunction::ease_out_sine,
         0.175f }
    });
    if (root["animations"])
    {
        auto animations_node = root["animations"];
        if (!animations_node.IsSequence())
        {
            mir::log_error("Unable to parse animations_node: animations_node is not an array");
            return;
        }

        for (auto const& node : animations_node)
        {
            auto const& event = try_parse_enum<AnimateableEvent>(
                node,
                "event",
                from_string_animateable_event,
                AnimateableEvent::max);
            if (event == AnimateableEvent::max)
                continue;

            auto const& type = try_parse_enum<AnimationType>(
                node,
                "type",
                from_string_animation_type,
                AnimationType::max);
            if (type == AnimationType::max)
                continue;

            auto const& function = try_parse_enum<EaseFunction>(
                node,
                "function",
                from_string_ease_function,
                EaseFunction::max);
            if (function == EaseFunction::max)
                continue;

            parsed[(int)event].type = type;
            parsed[(int)event].function = function;
            try_parse_value(node, "duration", parsed[(int)event].duration_seconds);
            try_parse_value(node, "c1", parsed[(int)event].c1);
            try_parse_value(node, "c2", parsed[(int)event].c2);
            try_parse_value(node, "c3", parsed[(int)event].c3);
            try_parse_value(node, "c4", parsed[(int)event].c4);
            try_parse_value(node, "n1", parsed[(int)event].n1);
            try_parse_value(node, "d1", parsed[(int)event].d1);
        }
    }

    animation_defintions = parsed;

    if (root["enable_animations"])
        try_parse_value(root, "enable_animations", animations_enabled);
}

void FilesystemConfiguration::_watch(miral::MirRunner& runner)
{
    inotify_fd = mir::Fd { inotify_init() };
    file_watch = inotify_add_watch(inotify_fd, config_path.c_str(), IN_MODIFY);
    if (file_watch < 0)
        mir::fatal_error("Unable to watch the config file");

    watch_handle = runner.register_fd_handler(inotify_fd, [&](int file_fd)
    {
        union
        {
            inotify_event event;
            char buffer[sizeof(inotify_event) + NAME_MAX + 1];
        } inotify_buffer;

        if (read(inotify_fd, &inotify_buffer, sizeof(inotify_buffer)) < static_cast<ssize_t>(sizeof(inotify_event)))
            return;

        if (inotify_buffer.event.mask & (IN_MODIFY))
        {
            _load();
            has_changes = true;
        }
    });
}

void FilesystemConfiguration::try_process_change()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!has_changes)
        return;

    has_changes = false;
    for (auto const& on_change : on_change_listeners)
    {
        on_change.listener(*this);
    }
}

uint FilesystemConfiguration::get_primary_modifier() const
{
    return primary_modifier;
}

uint FilesystemConfiguration::parse_modifier(std::string const& stringified_action_key)
{
    if (stringified_action_key == "alt")
        return mir_input_event_modifier_alt;
    else if (stringified_action_key == "alt_left")
        return mir_input_event_modifier_alt_left;
    else if (stringified_action_key == "alt_right")
        return mir_input_event_modifier_alt_right;
    else if (stringified_action_key == "shift")
        return mir_input_event_modifier_shift;
    else if (stringified_action_key == "shift_left")
        return mir_input_event_modifier_shift_left;
    else if (stringified_action_key == "shift_right")
        return mir_input_event_modifier_shift_right;
    else if (stringified_action_key == "sym")
        return mir_input_event_modifier_sym;
    else if (stringified_action_key == "function")
        return mir_input_event_modifier_function;
    else if (stringified_action_key == "ctrl")
        return mir_input_event_modifier_ctrl;
    else if (stringified_action_key == "ctrl_left")
        return mir_input_event_modifier_ctrl_left;
    else if (stringified_action_key == "ctrl_right")
        return mir_input_event_modifier_ctrl_right;
    else if (stringified_action_key == "meta")
        return mir_input_event_modifier_meta;
    else if (stringified_action_key == "meta_left")
        return mir_input_event_modifier_meta_left;
    else if (stringified_action_key == "meta_right")
        return mir_input_event_modifier_meta_right;
    else if (stringified_action_key == "caps_lock")
        return mir_input_event_modifier_caps_lock;
    else if (stringified_action_key == "num_lock")
        return mir_input_event_modifier_num_lock;
    else if (stringified_action_key == "scroll_lock")
        return mir_input_event_modifier_scroll_lock;
    else if (stringified_action_key == "primary")
        return miracle_input_event_modifier_default;
    else
        mir::log_error("Unable to process action_key: %s", stringified_action_key.c_str());
    return mir_input_event_modifier_none;
}

std::string const& FilesystemConfiguration::get_filename() const
{
    return config_path;
}

MirInputEventModifier FilesystemConfiguration::get_input_event_modifier() const
{
    return (MirInputEventModifier)primary_modifier;
}

CustomKeyCommand const*
FilesystemConfiguration::matches_custom_key_command(MirKeyboardAction action, int scan_code, unsigned int modifiers) const
{
    // TODO: Copy & paste
    for (auto const& command : custom_key_commands)
    {
        if (action != command.action)
            continue;

        auto command_modifiers = command.modifiers;
        if (command_modifiers & miracle_input_event_modifier_default)
            command_modifiers = command_modifiers & ~miracle_input_event_modifier_default | get_input_event_modifier();

        if (command_modifiers != modifiers)
            continue;

        if (scan_code == command.key)
            return &command;
    }

    return nullptr;
}

bool FilesystemConfiguration::matches_key_command(MirKeyboardAction action, int scan_code, unsigned int modifiers, std::function<bool(DefaultKeyCommand)> const& f) const
{
    for (int i = 0; i < DefaultKeyCommand::MAX; i++)
    {
        for (auto command : key_commands[i])
        {
            if (action != command.action)
                continue;

            auto command_modifiers = command.modifiers;
            if (command_modifiers & miracle_input_event_modifier_default)
                command_modifiers = command_modifiers & ~miracle_input_event_modifier_default | get_input_event_modifier();

            if (command_modifiers != modifiers)
                continue;

            if (scan_code == command.key)
            {
                if (f((DefaultKeyCommand)i))
                    return true;
            }
        }
    }

    return false;
}

int FilesystemConfiguration::get_inner_gaps_x() const
{
    return inner_gaps_x;
}

int FilesystemConfiguration::get_inner_gaps_y() const
{
    return inner_gaps_y;
}

int FilesystemConfiguration::get_outer_gaps_x() const
{
    return outer_gaps_x;
}

int FilesystemConfiguration::get_outer_gaps_y() const
{
    return outer_gaps_y;
}

const std::vector<StartupApp>& FilesystemConfiguration::get_startup_apps() const
{
    return startup_apps;
}

int FilesystemConfiguration::register_listener(std::function<void(miracle::MiracleConfig&)> const& func)
{
    return register_listener(func, 5);
}

int FilesystemConfiguration::register_listener(std::function<void(miracle::MiracleConfig&)> const& func, int priority)
{
    int handle = next_listener_handle++;

    for (auto it = on_change_listeners.begin(); it != on_change_listeners.end(); it++)
    {
        if (it->priority >= priority)
        {
            on_change_listeners.insert(it, { func, priority, handle });
            return handle;
        }
    }

    on_change_listeners.push_back({ func, priority, handle });
    return handle;
}

void FilesystemConfiguration::unregister_listener(int handle)
{
    for (auto it = on_change_listeners.begin(); it != on_change_listeners.end(); it++)
    {
        if (it->handle == handle)
        {
            on_change_listeners.erase(it);
            return;
        }
    }
}

std::optional<std::string> const& FilesystemConfiguration::get_terminal_command() const
{
    if (!terminal)
    {
        auto error_string = "Terminal program does not exist " + desired_terminal;
        mir::log_error("%s", error_string.c_str());
        NotifyNotification* n = notify_notification_new(
            "Terminal program does not exist",
            error_string.c_str(),
            nullptr);
        notify_notification_set_timeout(n, 5000);
        notify_notification_show(n, nullptr);
    }
    return terminal;
}

int FilesystemConfiguration::get_resize_jump() const
{
    return resize_jump;
}

std::vector<EnvironmentVariable> const& FilesystemConfiguration::get_env_variables() const
{
    return environment_variables;
}

BorderConfig const& FilesystemConfiguration::get_border_config() const
{
    return border_config;
}

std::array<AnimationDefinition, (int)AnimateableEvent::max> const& FilesystemConfiguration::get_animation_definitions() const
{
    return animation_defintions;
}

bool FilesystemConfiguration::are_animations_enabled() const
{
    return animations_enabled;
}

WorkspaceConfig FilesystemConfiguration::get_workspace_config(int key) const
{
    for (auto const& config : workspace_configs)
    {
        if (config.num == key)
            return config;
    }

    return { key, ContainerType::leaf };
}