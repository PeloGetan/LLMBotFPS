#pragma once
#include "raylib.h"
#include "Math.h"
#include "Weapon.h"
#include <string>
#include <vector>
#include <unordered_map>

struct WeaponPickup {
    Vector2 pos;
    WeaponType type;
};

enum class Route { Unknown, Mid, LeftFlank, RightFlank };
const char* routeName(Route r);

struct NavNode {
    std::string name;
    Vector2 pos;
    std::vector<int> neighbors; // indices into nodes
};

// Top-down tactical map: walls (also block line-of-sight), named positions,
// cover points and a waypoint navigation graph computed at load time.
class Map {
public:
    Map();

    Rectangle playArea{0, 0, 900, 720};
    std::vector<Rectangle> walls;      // dividers + crates, block movement & LoS
    std::vector<Rectangle> crates;     // subset used as cover (drawn differently)
    std::unordered_map<std::string, Vector2> named;
    std::vector<Vector2> coverPoints;  // good spots to hide behind crates
    std::vector<WeaponPickup> weaponSpawns; // weapons the player can pick up
    std::vector<NavNode> nodes;

    Vector2 pos(const std::string& name) const;

    // Line of sight between two world points (blocked by any wall).
    bool hasLineOfSight(Vector2 a, Vector2 b) const;

    // Does a circle at p (radius r) overlap any wall?
    bool collides(Vector2 p, float r) const;

    // Classify a world position into a route lane.
    Route routeOf(Vector2 p) const;

    int nearestNode(Vector2 p) const;

    // Position of the first pickup of a given weapon type. Returns false if none.
    bool weaponSpawnPos(WeaponType t, Vector2& out) const;

    // Path of node indices from start node to goal node (Dijkstra). Empty if none.
    std::vector<int> findPath(int startNode, int goalNode) const;

private:
    void buildNavGraph();
};
