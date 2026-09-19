#pragma once
#include <cstdint>
#include <unordered_map>

// The caller serializes access. Never evict a scene while assembling a pair:
// a later scene allocation must not remove the camera of an earlier scene.
template<class Snapshot> class SceneCache {
    struct Entry { Snapshot snapshot; uint64_t frame; };
    std::unordered_map<void*,Entry> entries;
public:
    void Store(void* scene,const Snapshot& snapshot,uint64_t frame) { entries[scene]={snapshot,frame}; }
    Snapshot Find(void* scene) const {
        auto it=entries.find(scene);return it==entries.end()?Snapshot{}:it->second.snapshot;
    }
    void Erase(void* scene) { entries.erase(scene); }
    size_t Size() const { return entries.size(); }
    void Complete(uint64_t frame) {
        // Keep two completed generations as well as any scenes already queued
        // for a future frame. Address reuse always replaces the previous tag.
        if(frame<2)return;
        for(auto it=entries.begin();it!=entries.end();) {
            if(it->second.frame<frame-1)it=entries.erase(it);else ++it;
        }
    }
};
