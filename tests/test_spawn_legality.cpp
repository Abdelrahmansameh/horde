#include "core/Math.h"
#include "core/Rng.h"
#include "game/level/Level.h"
#include "game/wave/WaveDirector.h"
#include "sim/SimWorld.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/chaff/ChaffSystem.h"
#include "sim/flowfield/FlowField.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace immune;
using namespace immune::sim;
using namespace immune::game;

namespace {
ChaffTuning spawn_tuning() {
    ChaffTuning t;
    auto& p=t.family[static_cast<u32>(PathogenFamily::Parasite)];
    // Shipped parasite footprint: 9-unit silhouette, .16 collision fraction,
    // 4.5 contact spacing; long-body wall clearance and +/-25% size jitter.
    p.radius=1.44f;p.contact_spacing=4.5f;p.wall_radius=2.6325f;p.size_jitter=.25f;
    p.collides=false;p.replication_rate=0;
    return t;
}
std::unique_ptr<SimWorld> l6_world() {
    LevelDef level;LevelLoader loader;
    REQUIRE(loader.load_file("assets/levels/campaign_06_circuit_board.json",level).ok);
    auto world=std::make_unique<SimWorld>();
    SimDesc d;d.seed=401017;d.max_chaff=256;d.world_bounds=level.world_bounds;
    d.sim_bounds=level_sim_bounds(level);d.chaff_tuning=spawn_tuning();
    world->init(d,nullptr);
    REQUIRE(loader.instantiate(level,*world).ok);
    return world;
}
bool legal(const ChaffSpawnGuard& g,Vec2 p,f32 clearance) {
    const auto c=g.mask->world_to_cell(p);
    return g.mask->walkable(c.x,c.y) && g.flow->reachable(p) && g.sdf->sample(p)>=clearance;
}
}

TEST_CASE("L6 sixty-parasite packing cannot strand births behind the off-board cap", "[sim][wave][spawn-sites]") {
    auto before=l6_world();auto after=l6_world();
    const ChaffSpawnGuard guard{&after->tissue(),&after->sdf(),&after->flow()};
    ChaffSpawnSiteStats sites;Rng old_rng(19),new_rng(19);
    constexpr Vec2 marker{155.0f,290.0f};
    REQUIRE(before->chaff_system().spawn_burst(before->chaff(),PathogenFamily::Parasite,
        marker,10,60,old_rng,kNoSquad,0,60,.73f)==60);
    REQUIRE(after->chaff_system().spawn_burst(after->chaff(),PathogenFamily::Parasite,
        marker,10,60,new_rng,kNoSquad,0,60,.73f,&guard,&sites)==60);
    CHECK(sites.reprojected>0);CHECK(sites.denied_geometry==0);
    CHECK(old_rng.raw_state()==new_rng.raw_state());
    u32 unsafe_before=0;
    for (usize i=0;i<60;++i) {
        const Vec2 old{before->chaff().pos_x[i],before->chaff().pos_y[i]};
        const Vec2 now{after->chaff().pos_x[i],after->chaff().pos_y[i]};
        const bool was_legal=legal(guard,old,2.6325f*1.25f);
        unsafe_before+=!was_legal;
        REQUIRE(legal(guard,now,2.6325f*1.25f));
        if (was_legal) CHECK(old==now);
    }
    CHECK(unsafe_before==sites.reprojected);
    // The old SDF recovery sends cap-wall births to the unreachable far apron.
    // The guarded formation keeps the same mass on the real approach instead.
    for (int tick=0;tick<120;++tick) {before->tick();after->tick();}
    u32 stranded_before=0,stranded_after=0;
    for (usize i=0;i<before->chaff().count();++i)
        stranded_before+=!before->flow().reachable({before->chaff().pos_x[i],before->chaff().pos_y[i]});
    for (usize i=0;i<after->chaff().count();++i)
        stranded_after+=!after->flow().reachable({after->chaff().pos_x[i],after->chaff().pos_y[i]});
    CHECK(stranded_before>0);CHECK(stranded_after==0);
    CHECK(after->chaff().count()==60);
}

TEST_CASE("legal expanded mixed-family formations keep their packing and RNG", "[sim][spawn][spawn-sites]") {
    TissueMask mask;mask.resize(300,300,1,Vec2{});
    for (i32 y=0;y<300;++y) for (i32 x=0;x<300;++x) mask.set_walkable(x,y,true);
    DistanceField sdf;sdf.bake(mask);FlowField flow;FlowFieldBakeDesc bake;
    bake.goals.push_back({{280,150},{2,2}});flow.bake(mask,bake);
    const ChaffSpawnGuard guard{&mask,&sdf,&flow};
    auto tuning=spawn_tuning();
    for (u32 f=0;f<kFamilyCount;++f) tuning.family[f].replication_rate=0;
    ChaffSystem system;system.set_tuning(tuning);
    ChaffBuffers old,now;old.reserve(256);now.reserve(256);
    Rng old_rng(811),new_rng(811);ChaffSpawnSiteStats sites;
    bool expanded=false;
    for (u32 f=0;f<kFamilyCount;++f) {
        const Vec2 marker{75.0f+static_cast<f32>(f%2)*150,75.0f+static_cast<f32>(f/2)*150};
        const auto family=static_cast<PathogenFamily>(f);
        REQUIRE(system.spawn_burst(old,family,marker,10,60,old_rng)==60);
        REQUIRE(system.spawn_burst(now,family,marker,10,60,new_rng,kNoSquad,0,0,-1,&guard,&sites)==60);
        for (usize i=f*60;i<(f+1)*60;++i) {
            CHECK(old.pos_x[i]==now.pos_x[i]);CHECK(old.pos_y[i]==now.pos_y[i]);
            expanded|=math::length_sq(Vec2{now.pos_x[i],now.pos_y[i]}-marker)>100;
        }
    }
    CHECK(expanded);CHECK(sites.reprojected==0);CHECK(sites.denied_geometry==0);
    CHECK(old_rng.raw_state()==new_rng.raw_state());
    CHECK(old_rng.raw_inc()==new_rng.raw_inc());
    for (usize a=0;a<now.count();++a) for (usize b=a+1;b<now.count();++b) {
        const auto& pa=tuning.family[now.family[a]];const auto& pb=tuning.family[now.family[b]];
        const f32 contact=(pa.radius*pa.contact_spacing+pb.radius*pb.contact_spacing)*.5f;
        const Vec2 d{now.pos_x[a]-now.pos_x[b],now.pos_y[a]-now.pos_y[b]};
        REQUIRE(math::length_sq(d)>=contact*contact);
    }
}

TEST_CASE("impossible guarded spawn geometry reports a shortfall without free mass", "[sim][wave][spawn-sites]") {
    TissueMask mask;mask.resize(32,32,1,Vec2{});DistanceField sdf;sdf.bake(mask);
    FlowField flow;FlowFieldBakeDesc bake;bake.goals.push_back({{25,16},{1,1}});flow.bake(mask,bake);
    const ChaffSpawnGuard guard{&mask,&sdf,&flow};
    ChaffSystem system;system.set_tuning(spawn_tuning());
    ChaffBuffers old,now;old.reserve(128);now.reserve(128);Rng a(34),b(34);ChaffSpawnSiteStats sites;
    REQUIRE(system.spawn_burst(old,PathogenFamily::Parasite,{16,16},10,60,a)==60);
    CHECK(system.spawn_burst(now,PathogenFamily::Parasite,{16,16},10,60,b,kNoSquad,0,0,-1,&guard,&sites)==0);
    CHECK(now.count()==0);CHECK(sites.reprojected==0);CHECK(sites.denied_geometry==60);
    CHECK(a.raw_state()==b.raw_state());
}

TEST_CASE("WaveDirector keeps authored spawn accounting when geometry is impossible", "[wave][spawn-sites]") {
    LevelLoader loader;const auto level=LevelLoader::default_test_level();SimWorld world;
    SimDesc d;d.max_chaff=128;d.world_bounds=level.world_bounds;world.init(d,nullptr);
    REQUIRE(loader.instantiate(level,world).ok);
    auto& mask=world.tissue();
    for (i32 y=0;y<mask.height();++y) for (i32 x=0;x<mask.width();++x) mask.set_walkable(x,y,false);
    world.sdf().bake(mask);FlowFieldBakeDesc bake;bake.goals.push_back({{20,20},{1,1}});world.flow().bake(mask,bake);
    WaveDef wave;wave.prep_time=0;SpawnEntry entry;entry.family=PathogenFamily::Parasite;
    entry.count=60;entry.duration=.1f;wave.spawns.push_back(entry);
    WaveDirector waves;waves.set_waves({wave});waves.start(world);Rng rng(1);
    for (int tick=0;tick<20;++tick) waves.tick(world,rng,kFixedDt);
    REQUIRE(waves.metrics().size()==1);const auto& m=waves.metrics()[0];
    CHECK(m.requested==60);CHECK(m.spawned==0);CHECK(m.denied==60);
    CHECK(m.spawn_sites_denied_geometry==60);CHECK(world.chaff().count()==0);
}
