#pragma once
//
// RemoteValue.h -- what travels between the server's and the client's Lua
// states (RemoteEvent / RemoteFunction arguments and results, BindableEvent
// payloads). Like Roblox, values are copied: tables become new tables,
// Instances are sent by id (an Instance the other side does not have arrives
// as nil), and functions, threads and metatables are dropped.
//
#include "scene/Value.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace rift {

enum class NetworkSide : uint8_t { Server, Client };

struct RemoteValue {
    enum class Type : uint8_t { Nil, Bool, Number, String, Instance, Vector3, Vector2, Color3, UDim, UDim2, CFrame, Enum, Table };
    Type type = Type::Nil;
    bool boolean = false;
    double number = 0.0;
    std::string text;              // String; Enum: the enum type's name
    uint64_t id = 0;               // Instance
    glm::vec3 v3{ 0.0f };          // Vector3; CFrame position
    glm::vec2 v2{ 0.0f };          // Vector2
    glm::mat3 rotation{ 1.0f };    // CFrame
    Color3 color;                  // Color3
    float udim[4] = { 0, 0, 0, 0 };// UDim (2 used) / UDim2
    int enumValue = 0;             // Enum
    std::vector<RemoteValue> keys, values; // Table
};

struct RemoteMessage {
    enum class Kind : uint8_t { Event, Invoke, Reply };
    Kind kind = Kind::Event;
    NetworkSide to = NetworkSide::Server;
    uint64_t remote = 0;  // the RemoteEvent / RemoteFunction
    uint64_t call = 0;    // Invoke / Reply: matches the answer to the caller waiting for it
    bool ok = true;       // Reply: false = the handler failed (error holds why)
    std::string error;
    std::vector<RemoteValue> args;
};

} // namespace rift
