#pragma once
//
// Replicator.h -- keeps the client's copy of the game world in step with the
// server's, like Roblox's replication:
//
//   * Everything the server has is copied to the client with the same ids,
//     except ServerStorage and ScriptService (server-only, never sent).
//   * Server changes (new / destroyed / moved instances, properties,
//     attributes, tags) are applied to the client as they happen, so the
//     client's scripts see ChildAdded, Changed, ... for them.
//   * Physics moves parts without property events; syncTransforms() copies
//     the transforms that changed on the server since the last frame (a
//     client that moved a part locally keeps that until the server moves it).
//   * What the client creates or changes stays on the client, except for the
//     local character, which the client owns: its parts' transforms and its
//     Humanoid's movement settings go back to the server (network ownership).
//
#include "scene/Scene.h"

#include <unordered_map>

namespace rift {

class Replicator {
public:
    Replicator(Scene& server, Scene& client);
    ~Replicator();
    Replicator(const Replicator&) = delete;
    Replicator& operator=(const Replicator&) = delete;

    // Copies the server's world to the (empty) client scene and starts listening.
    void start();
    // Per frame, after physics: moved parts and changed terrain chunks.
    void syncTransforms();
    // The character the client owns (0 = none).
    void setLocalCharacter(uint64_t id) { character_ = id; }

    // True if `i` (in the server scene) is sent to the client.
    static bool replicates(const Scene& server, const Instance& i);

private:
    struct ServerObserver : SceneObserver {
        Replicator* r = nullptr;
        void onInstanceAdded(Instance& i) override { r->added(i); }
        void onInstanceRemoving(Instance& i) override { r->removing(i); }
        void onParentChanged(Instance& i, Instance* old) override { r->parentChanged(i, old); }
        void onPropertyChanged(Instance& i, const PropertyInfo& p) override { r->propertyChanged(i, p); }
        void onAttributeChanged(Instance& i, const std::string& name) override { r->attributeChanged(i, name); }
        void onTagsChanged(Instance& i) override { r->tagsChanged(i); }
    };
    struct ClientObserver : SceneObserver {
        Replicator* r = nullptr;
        void onPropertyChanged(Instance& i, const PropertyInfo& p) override { r->clientPropertyChanged(i, p); }
    };
    struct Sent { glm::vec3 position, rotation, velocity, angular; };

    Scene& server_;
    Scene& client_;
    ServerObserver serverObserver_;
    ClientObserver clientObserver_;
    bool started_ = false;
    int applying_ = 0;           // > 0 while copying in either direction (no echo)
    uint64_t character_ = 0;
    std::unordered_map<uint64_t, Sent> sent_; // last transform sent per part
    uint64_t terrainRevision_ = 0;            // the server terrain's revision last sent
    std::unordered_map<ChunkKey, uint64_t, ChunkKeyHash> terrainSent_; // server chunk revisions sent

    void added(Instance& i);
    void removing(Instance& i);
    void parentChanged(Instance& i, Instance* oldParent);
    void propertyChanged(Instance& i, const PropertyInfo& p);
    void attributeChanged(Instance& i, const std::string& name);
    void tagsChanged(Instance& i);
    void clientPropertyChanged(Instance& i, const PropertyInfo& p);

    // Makes the client match the server for `i` (creates, moves or removes it).
    void place(const Instance& i);
    void remember(const Instance& i);
};

} // namespace rift
