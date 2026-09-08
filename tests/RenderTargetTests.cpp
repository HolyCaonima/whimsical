#include "core/World.h"
#include "core/FrameMailbox.h"
#include "assets/EngineAssets.h"
#include "scene/ScenePersistence.h"
#include "scripting/ScriptRuntime.h"
#include <cstring>
#include <iostream>
#include <thread>

using namespace afterlight;
static void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    try {
        auto directory = std::filesystem::path(AFTERLIGHT_ROOT) / "build" / ("rt-" + newPersistentId());
        AssetManager assets{Project::create(directory, "RenderTarget contracts").content()};
        registerEngineAssets(assets);
        AssetHeader h;
        h.id = newPersistentId();
        h.type = "RenderTarget";
        h.name = "Test RT";
        auto ref = assets.save(AssetPath("/Game/Target"), h, R"({"width":2,"height":1,"format":"R32Uint"})");
        World world;
        ScriptRuntime js(world, assets);
        js.execute(R"(
            function expect(v, why) { if (!v) throw new Error(why); }
            var asset = Engine.asset('/Game/Target');
            var rt = Engine.renderTarget(asset);
            var info = Engine.renderTargetInfo(rt);
            expect(info.descriptorWidth === 2 && info.descriptorHeight === 1, 'descriptor dimensions');
            expect(info.format === 'R32Uint' && info.status === 'uninitialized', 'format/status');
            var writer = Engine.create({components: {drawEntityID: {target: asset}}});
            expect(Engine.component(writer, 'drawEntityID').target.id === asset.id, 'generic asset ref');
            var ticket = Engine.readPixels(rt);
            expect(Engine.pollPixels(ticket) === null, 'must not manufacture pixels');
        )");
        Input input;
        input.width = 2;
        input.height = 1;
        auto first = world.snapshot(input, 10, 0, 0);
        check(first.entityIDOutputs.size() == 1 && first.pixelReads.size() == 1, "ECS extraction");
        auto resource = first.entityIDOutputs[0];
        auto request = first.pixelReads[0];
        auto scene = ScenePersistence::capture(world, assets);
        auto decoded = SceneDocument::fromJson(Json::parse(scene.json().dump()));
        check(decoded.entities[0].components.find<SceneDrawEntityID>()->target.id == ref.id,
              "Only the asset reference is serialized");
        // Discard an unread snapshot, then present the delivered snapshot twice.
        FrameMailbox mailbox;
        mailbox.publish(std::move(first));
        mailbox.publish(world.snapshot(input, 12, 0, 0));
        FrameRef delivered;
        uint64_t seen = 0;
        check(mailbox.acquire(delivered, seen) == FrameStatus::Fresh, "delivery");
        check(delivered->tick == 12 && delivered->pixelReads.at(0) == request && request->requestedTick == 10,
              "Dropped snapshots retain reads and their original request tick");
        check(mailbox.acquire(delivered, seen) == FrameStatus::Repeat, "repeat");
        std::thread consumer([request, resource] {
            check(request->claim() && !request->claim(), "Repeated snapshots must submit exactly once");
            TextureContents contents{123, 4, 27, 12, 2, 1};
            PixelReadResult result{"ready", contents, {0, 0, 2, 1}, std::vector<uint8_t>(8)};
            const uint32_t values[] = {0xfedcba98u, 0};
            std::memcpy(result.bytes.data(), values, sizeof(values));
            resource->publish(contents);
            request->complete(std::move(result));
        });
        consumer.join();
        js.execute(R"(
            var result = Engine.pollPixels(ticket);
            expect(result.status === 'ready' && result.data instanceof Uint32Array, 'typed delivery');
            expect(result.data[0] === 4275878552 && result.data[1] === 0, 'all 32 bits, including high IDs');
            expect(result.sourceTick === '12' && result.requestedTick === '10' && result.renderFrame === '27',
                   'producer metadata, not the requesting frame');
            expect(result.rtVersion === '123' && result.contentVersion === '4', 'content identity');
            expect(Engine.renderTargetInfo(rt).rtVersion === result.rtVersion, 'completed metadata');
            var cancelled = Engine.readPixels(rt);
        )");
        auto pending = world.snapshot(input, 13, 0, 0).pixelReads.at(0);
        check(pending->claim(), "in-flight read");
        js.execute("Engine.releaseRenderTarget(rt);");
        pending->complete({"ready", {}, {}, {1, 2, 3, 4}});
        js.execute("expect(Engine.pollPixels(cancelled).status === 'invalidated', 'cancellation wins');");
        check(!resource->valid(), "Retained snapshots cannot revive invalidated RTs");
        auto oldAsset = assets.load<RenderTargetAsset>(ref);
        assets.clearCache();
        auto newAsset = assets.load<RenderTargetAsset>(ref);
        check(newAsset->generation != oldAsset->generation, "Descriptor load generations are distinct");
        auto oldTarget = world.renderTargets.acquire(oldAsset),
             newTarget = world.renderTargets.acquire(newAsset);
        check(oldTarget != newTarget, "Reloaded descriptions must not alias old GPU contents");
        auto orphan = world.renderTargets.read(oldTarget->handle, {});
        auto held = world.renderTargets.request(orphan);
        world.clearScene();
        check(!oldTarget->valid() && !newTarget->valid() && held->take()->status == "invalidated",
              "Scene teardown cancels pending work even while old snapshots are retained");
        // General RT descriptions are valid; the integer-only restriction belongs to the producer.
        auto floatAsset = RenderTargetAsset::decode(R"({"width":0,"height":0,"format":"RGBA32Float"})");
        check(floatAsset->format == PixelFormat::RGBA32Float && pixelBytes(floatAsset->format) == 16,
              "RT format support is independent of EntityID");
        std::cout << "RenderTarget asset, ECS, snapshot, cross-thread and JS contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
