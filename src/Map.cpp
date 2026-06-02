#include "Map.h"
#include <algorithm>
#include <limits>
#include <queue>

const char* routeName(Route r) {
    switch (r) {
        case Route::Mid: return "mid";
        case Route::LeftFlank: return "left_flank";
        case Route::RightFlank: return "right_flank";
        default: return "unknown";
    }
}

// Segment (p1->p2) vs axis-aligned rectangle intersection (slab method).
static bool segIntersectsRect(Vector2 p1, Vector2 p2, Rectangle r) {
    Vector2 d = p2 - p1;
    float tmin = 0.0f, tmax = 1.0f;
    const float minx = r.x, maxx = r.x + r.width;
    const float miny = r.y, maxy = r.y + r.height;

    // X slab
    if (std::fabs(d.x) < 1e-8f) {
        if (p1.x < minx || p1.x > maxx) return false;
    } else {
        float inv = 1.0f / d.x;
        float t1 = (minx - p1.x) * inv;
        float t2 = (maxx - p1.x) * inv;
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return false;
    }
    // Y slab
    if (std::fabs(d.y) < 1e-8f) {
        if (p1.y < miny || p1.y > maxy) return false;
    } else {
        float inv = 1.0f / d.y;
        float t1 = (miny - p1.y) * inv;
        float t2 = (maxy - p1.y) * inv;
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return false;
    }
    return true;
}

Map::Map() {
    // ---- Walls: two vertical dividers split the arena into 3 lanes,
    //      each with a doorway in the middle that links neighbouring lanes. ----
    walls.push_back({250, 120, 40, 210});  // left divider, upper
    walls.push_back({250, 400, 40, 200});  // left divider, lower (gap 330..400)
    walls.push_back({610, 120, 40, 210});  // right divider, upper
    walls.push_back({610, 400, 40, 200});  // right divider, lower

    // ---- Crates (cover, also block LoS) ----
    crates.push_back({425, 335, 50, 50});  // mid_box crate
    crates.push_back({335, 495, 55, 55});  // off-angle crate
    crates.push_back({120, 330, 50, 50});  // left flank crate
    crates.push_back({730, 330, 50, 50});  // right flank crate
    crates.push_back({720, 515, 55, 55});  // long-angle crate
    for (auto& c : crates) walls.push_back(c);

    // ---- Named positions (telemetry + LLM summaries) ----
    named["player_spawn"]    = {450, 55};
    named["bot_spawn"]       = {450, 665};
    named["mid_entry"]       = {450, 160};
    named["mid_box"]         = {450, 300};
    named["left_flank"]      = {150, 300};
    named["right_flank"]     = {750, 300};
    named["long_angle"]      = {760, 470};
    named["crate_off_angle"] = {380, 470};
    named["back_site"]       = {450, 605};
    // Helper junction waypoints to keep the nav graph connected.
    named["top_left"]        = {150, 150};
    named["top_right"]       = {750, 150};
    named["mid_gap_left"]    = {320, 365};
    named["mid_gap_right"]   = {580, 365};
    named["bottom_left"]     = {150, 560};
    named["bottom_right"]    = {750, 560};
    named["bottom_junction"] = {450, 640};

    // ---- Cover points (where the bot can stand shielded by a crate) ----
    coverPoints = {
        {450, 320}, // behind mid crate (facing player)
        {380, 480}, // off-angle behind crate
        {150, 320}, // left flank cover
        {750, 320}, // right flank cover
        {760, 500}, // long-angle cover
    };

    // ---- Weapon pickups: down each flank, so grabbing one means committing
    //      to a route (a real tactical choice). ----
    weaponSpawns.push_back({{150, 470}, WeaponType::Rifle});
    weaponSpawns.push_back({{760, 560}, WeaponType::Shotgun});

    buildNavGraph();
}

Vector2 Map::pos(const std::string& name) const {
    auto it = named.find(name);
    if (it != named.end()) return it->second;
    return {playArea.width * 0.5f, playArea.height * 0.5f};
}

bool Map::hasLineOfSight(Vector2 a, Vector2 b) const {
    for (const auto& w : walls)
        if (segIntersectsRect(a, b, w)) return false;
    return true;
}

bool Map::collides(Vector2 p, float r) const {
    if (p.x - r < playArea.x || p.x + r > playArea.x + playArea.width) return true;
    if (p.y - r < playArea.y || p.y + r > playArea.y + playArea.height) return true;
    for (const auto& w : walls) {
        float cx = clampf(p.x, w.x, w.x + w.width);
        float cy = clampf(p.y, w.y, w.y + w.height);
        if (vDistSq(p, {cx, cy}) < r * r) return true;
    }
    return false;
}

Route Map::routeOf(Vector2 p) const {
    if (p.x < 250) return Route::LeftFlank;
    if (p.x > 650) return Route::RightFlank;
    return Route::Mid;
}

void Map::buildNavGraph() {
    static const char* nodeNames[] = {
        "player_spawn", "bot_spawn", "mid_entry", "mid_box", "left_flank",
        "right_flank", "long_angle", "crate_off_angle", "back_site",
        "top_left", "top_right", "mid_gap_left", "mid_gap_right",
        "bottom_left", "bottom_right", "bottom_junction"};

    for (const char* n : nodeNames)
        nodes.push_back({n, named[n], {}});

    const float maxEdge = 360.0f;
    for (int i = 0; i < (int)nodes.size(); ++i) {
        for (int j = i + 1; j < (int)nodes.size(); ++j) {
            float d = vDist(nodes[i].pos, nodes[j].pos);
            if (d <= maxEdge && hasLineOfSight(nodes[i].pos, nodes[j].pos)) {
                nodes[i].neighbors.push_back(j);
                nodes[j].neighbors.push_back(i);
            }
        }
    }
}

int Map::nearestNode(Vector2 p) const {
    int best = 0;
    float bestD = std::numeric_limits<float>::max();
    for (int i = 0; i < (int)nodes.size(); ++i) {
        // Prefer reachable (LoS) nodes; fall back to plain distance.
        float d = vDistSq(p, nodes[i].pos);
        bool los = hasLineOfSight(p, nodes[i].pos);
        if (los) d *= 0.5f;
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

std::vector<int> Map::findPath(int startNode, int goalNode) const {
    int n = (int)nodes.size();
    std::vector<float> dist(n, std::numeric_limits<float>::max());
    std::vector<int> prev(n, -1);
    using QE = std::pair<float, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
    dist[startNode] = 0;
    pq.push({0, startNode});
    while (!pq.empty()) {
        auto [d, u] = pq.top();
        pq.pop();
        if (d > dist[u]) continue;
        if (u == goalNode) break;
        for (int v : nodes[u].neighbors) {
            float nd = d + vDist(nodes[u].pos, nodes[v].pos);
            if (nd < dist[v]) {
                dist[v] = nd;
                prev[v] = u;
                pq.push({nd, v});
            }
        }
    }
    std::vector<int> path;
    if (dist[goalNode] == std::numeric_limits<float>::max()) return path;
    for (int at = goalNode; at != -1; at = prev[at]) path.push_back(at);
    std::reverse(path.begin(), path.end());
    return path;
}
