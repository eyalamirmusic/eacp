#include <eacp/GPU/GPU.h>
#include <algorithm>

using namespace eacp;
using namespace GPU;

// A Wolfenstein3D-style first-person walk through a grid maze. The map below
// extrudes into textured wall quads once at startup; the camera is two scalar
// uniforms (position and yaw) the shader builds the view matrix from each
// frame. Move with W/A/S/D, turn with the arrow keys — or click to lock the
// mouse for mouse look; Escape releases it.
namespace
{
// One character per cell: '.' is walkable, anything else is a wall whose
// character picks the texture. Rows run along Z, columns along X.
constexpr std::string_view worldMap[] = {
    "########################",
    "#......#.......%.......%",
    "#..==..#..###..%..%%...%",
    "#..==..#..#....%...%...%",
    "#......#..#.##.%%%.%...%",
    "###.####..#..#.....%...%",
    "#.........####.%%%%%...%",
    "#...####.......%.......%",
    "#...#..#########.%%%%%%%",
    "#.........#............#",
    "#####..#..#..%%%%..#...#",
    "#......#..#........#...#",
    "#..#####..#####..###...#",
    "#..#...........#.......#",
    "#......#####...#...#...#",
    "########################",
};

constexpr auto mapRows = (int) std::size(worldMap);

constexpr float pi = 3.14159265358979f;

float radians(float degrees)
{
    return degrees * (pi / 180.0f);
}

bool isSolid(int x, int z)
{
    if (z < 0 || z >= mapRows)
        return true;

    const auto& row = worldMap[z];

    if (x < 0 || x >= (int) row.size())
        return true;

    return row[x] != '.';
}

// Atlas tile per wall character; the last two tiles are the flat floor and
// ceiling colours.
constexpr auto brickTile = 0;
constexpr auto stoneTile = 1;
constexpr auto plankTile = 2;
constexpr auto floorTile = 3;
constexpr auto ceilingTile = 4;
constexpr auto tileCount = 5;
constexpr auto tileSize = 64;

int wallTile(char cell)
{
    switch (cell)
    {
        case '%':
            return stoneTile;
        case '=':
            return plankTile;
        default:
            return brickTile;
    }
}

std::uint32_t rgb(int r, int g, int b)
{
    return 0xff000000u | (std::uint32_t) b << 16 | (std::uint32_t) g << 8
           | (std::uint32_t) r;
}

// Deterministic per-cell variation so bricks and planks don't look flat.
std::uint32_t hash2(int x, int y)
{
    auto h = (std::uint32_t) (x * 374761393 + y * 668265263);
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

int vary(int base, int x, int y, int amount)
{
    return base + (int) (hash2(x, y) % (std::uint32_t) (2 * amount + 1)) - amount;
}

std::uint32_t brickPixel(int x, int y)
{
    constexpr auto brickHeight = 16;
    constexpr auto brickWidth = 32;

    auto row = y / brickHeight;
    auto shifted = (x + (row % 2) * brickWidth / 2) % tileSize;

    if (y % brickHeight < 2 || shifted % brickWidth < 2)
        return rgb(110, 104, 100);

    auto tone = vary(0, shifted / brickWidth, row, 14);
    return rgb(165 + tone, 58 + tone / 2, 45 + tone / 2);
}

std::uint32_t stonePixel(int x, int y)
{
    constexpr auto blockSize = 16;

    if (x % blockSize == 0 || y % blockSize == 0)
        return rgb(58, 60, 64);

    auto tone = vary(0, x / blockSize, y / blockSize, 16);
    return rgb(128 + tone, 130 + tone, 134 + tone);
}

std::uint32_t plankPixel(int x, int y)
{
    constexpr auto plankWidth = 16;

    if (x % plankWidth == 0)
        return rgb(62, 40, 24);

    auto plank = x / plankWidth;
    auto tone = vary(0, plank, 0, 12) + vary(0, plank, y / 3, 6);
    return rgb(140 + tone, 92 + tone / 2, 52 + tone / 3);
}

std::uint32_t tilePixel(int tile, int x, int y)
{
    switch (tile)
    {
        case brickTile:
            return brickPixel(x, y);
        case stoneTile:
            return stonePixel(x, y);
        case plankTile:
            return plankPixel(x, y);
        case floorTile:
            return rgb(96, 92, 86);
        default:
            return rgb(56, 58, 64);
    }
}

// All tiles side by side in one texture, sampled with nearest filtering for
// the chunky retro look.
Texture makeAtlas(Device& device)
{
    static std::uint32_t pixels[tileSize * tileSize * tileCount];

    for (auto tile = 0; tile < tileCount; ++tile)
        for (auto y = 0; y < tileSize; ++y)
            for (auto x = 0; x < tileSize; ++x)
                pixels[y * tileSize * tileCount + tile * tileSize + x] =
                    tilePixel(tile, x, y);

    auto descriptor = TextureDescriptor {};
    descriptor.width = tileSize * tileCount;
    descriptor.height = tileSize;
    descriptor.filter = TextureFilter::Nearest;

    return device.makeTexture(descriptor, pixels);
}

struct Vertex
{
    float position[3];
    float uv[2];
    float shade;
};

struct MazeMesh
{
    Vector<Vertex> vertices;
    Vector<std::uint32_t> indices;
};

// Maps a 0..1 coordinate across a face into the tile's strip of the atlas,
// inset by half a texel so neighbouring tiles never bleed in.
float tileU(int tile, float u)
{
    constexpr auto inset = 0.5f / tileSize;
    return ((float) tile + inset + u * (1.0f - 2.0f * inset)) / tileCount;
}

void addQuad(MazeMesh& mesh,
             const Vertex& a,
             const Vertex& b,
             const Vertex& c,
             const Vertex& d)
{
    auto base = (std::uint32_t) mesh.vertices.size();

    mesh.vertices.add(a);
    mesh.vertices.add(b);
    mesh.vertices.add(c);
    mesh.vertices.add(d);

    mesh.indices.getVector().insert(
        mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

// One unit-square wall face per solid cell edge that borders a walkable cell.
// North/south faces are fully lit and east/west ones darker, the classic
// Wolfenstein two-level shading that keeps corners readable.
void addWallFaces(MazeMesh& mesh, int x, int z)
{
    auto tile = wallTile(worldMap[z][(std::size_t) x]);
    auto u0 = tileU(tile, 0.0f);
    auto u1 = tileU(tile, 1.0f);

    auto fx = (float) x;
    auto fz = (float) z;

    constexpr auto bright = 1.0f;
    constexpr auto dark = 0.7f;

    if (!isSolid(x, z - 1))
        addQuad(mesh,
                {{fx, 1.0f, fz}, {u0, 0.0f}, bright},
                {{fx + 1.0f, 1.0f, fz}, {u1, 0.0f}, bright},
                {{fx + 1.0f, 0.0f, fz}, {u1, 1.0f}, bright},
                {{fx, 0.0f, fz}, {u0, 1.0f}, bright});

    if (!isSolid(x, z + 1))
        addQuad(mesh,
                {{fx + 1.0f, 1.0f, fz + 1.0f}, {u0, 0.0f}, bright},
                {{fx, 1.0f, fz + 1.0f}, {u1, 0.0f}, bright},
                {{fx, 0.0f, fz + 1.0f}, {u1, 1.0f}, bright},
                {{fx + 1.0f, 0.0f, fz + 1.0f}, {u0, 1.0f}, bright});

    if (!isSolid(x - 1, z))
        addQuad(mesh,
                {{fx, 1.0f, fz + 1.0f}, {u0, 0.0f}, dark},
                {{fx, 1.0f, fz}, {u1, 0.0f}, dark},
                {{fx, 0.0f, fz}, {u1, 1.0f}, dark},
                {{fx, 0.0f, fz + 1.0f}, {u0, 1.0f}, dark});

    if (!isSolid(x + 1, z))
        addQuad(mesh,
                {{fx + 1.0f, 1.0f, fz}, {u0, 0.0f}, dark},
                {{fx + 1.0f, 1.0f, fz + 1.0f}, {u1, 0.0f}, dark},
                {{fx + 1.0f, 0.0f, fz + 1.0f}, {u1, 1.0f}, dark},
                {{fx + 1.0f, 0.0f, fz}, {u0, 1.0f}, dark});
}

// The floor and ceiling are single flat-coloured quads under and over the
// whole map, sampling the centre of their solid atlas tiles.
void addFloorAndCeiling(MazeMesh& mesh, float width, float depth)
{
    auto floorUV = Graphics::Point {tileU(floorTile, 0.5f), 0.5f};
    auto ceilingUV = Graphics::Point {tileU(ceilingTile, 0.5f), 0.5f};

    addQuad(mesh,
            {{0.0f, 0.0f, 0.0f}, {floorUV.x, floorUV.y}, 1.0f},
            {{width, 0.0f, 0.0f}, {floorUV.x, floorUV.y}, 1.0f},
            {{width, 0.0f, depth}, {floorUV.x, floorUV.y}, 1.0f},
            {{0.0f, 0.0f, depth}, {floorUV.x, floorUV.y}, 1.0f});

    addQuad(mesh,
            {{0.0f, 1.0f, depth}, {ceilingUV.x, ceilingUV.y}, 1.0f},
            {{width, 1.0f, depth}, {ceilingUV.x, ceilingUV.y}, 1.0f},
            {{width, 1.0f, 0.0f}, {ceilingUV.x, ceilingUV.y}, 1.0f},
            {{0.0f, 1.0f, 0.0f}, {ceilingUV.x, ceilingUV.y}, 1.0f});
}

MazeMesh buildMaze()
{
    auto mesh = MazeMesh {};
    auto maxWidth = 0;

    for (auto z = 0; z < mapRows; ++z)
    {
        maxWidth = std::max(maxWidth, (int) worldMap[z].size());

        for (auto x = 0; x < (int) worldMap[z].size(); ++x)
            if (isSolid(x, z))
                addWallFaces(mesh, x, z);
    }

    addFloorAndCeiling(mesh, (float) maxWidth, (float) mapRows);
    return mesh;
}

// The whole camera is three scalar uniforms; the shader assembles the view
// matrix (yaw spin around the eye, then the eye translation) and projection
// itself, so the CPU never touches a matrix.
struct MazeShader final : ShaderProgram
{
    MazeShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&Vertex::position);
        auto uv = vertexInput(&Vertex::uv);
        auto shade = vertexInput(&Vertex::shade);

        auto view = rotateY(-yaw) * translate(-camX, constant(-0.5f), -camZ);
        auto projection = perspective(aspect, radians(65.0f), 0.05f, 100.0f);
        setPosition(projection * view * float4(position, 1.0f));

        auto color = sample(atlas, varying(uv));
        setFragment(float4(color.xyz() * varying(shade), 1.0f));
    }

    Uniform<Float> camX;
    Uniform<Float> camZ;
    Uniform<Float> yaw;
    Uniform<Float> aspect;
    Uniform<Texture2D> atlas;

    EACP_SHADER(camX, camZ, yaw, aspect, atlas)
};
} // namespace

struct MazeView final : GPUView
{
    MazeView()
        : mesh(buildMaze())
        , atlas(makeAtlas(Device::shared()))
    {
        setDepth(true);
        setHandlesMouseEvents(true);
        shader.setVertices(mesh.vertices.data(), mesh.vertices.size());
        shader.setIndices(mesh.indices.data(), mesh.indices.size());
        shader.prepare(sampleCount(), true);
        shader.atlas = atlas;
        setContinuous(true);
    }

    void mouseDown(const Graphics::MouseEvent&) override
    {
        if (window != nullptr)
            window->setMouseLocked(true);
    }

    void mouseMoved(const Graphics::MouseEvent& event) override
    {
        turnWithMouse(event);
    }

    void mouseDragged(const Graphics::MouseEvent& event) override
    {
        turnWithMouse(event);
    }

    void turnWithMouse(const Graphics::MouseEvent& event)
    {
        if (window != nullptr && window->isMouseLocked())
            yaw -= event.delta.x * mouseSensitivity;
    }

    void update(Threads::FrameTime time) override
    {
        using namespace Graphics;

        auto delta = (float) time.delta;

        if (window != nullptr && Keyboard::isKeyPressed(KeyCode::Escape))
            window->setMouseLocked(false);

        auto turn = 0.0f;
        if (Keyboard::isKeyPressed(KeyCode::LeftArrow))
            turn += 1.0f;
        if (Keyboard::isKeyPressed(KeyCode::RightArrow))
            turn -= 1.0f;

        yaw += turn * turnSpeed * delta;

        auto forward = 0.0f;
        if (Keyboard::isKeyPressed(KeyCode::W)
            || Keyboard::isKeyPressed(KeyCode::UpArrow))
            forward += 1.0f;
        if (Keyboard::isKeyPressed(KeyCode::S)
            || Keyboard::isKeyPressed(KeyCode::DownArrow))
            forward -= 1.0f;

        auto strafe = 0.0f;
        if (Keyboard::isKeyPressed(KeyCode::D))
            strafe += 1.0f;
        if (Keyboard::isKeyPressed(KeyCode::A))
            strafe -= 1.0f;

        auto sinYaw = std::sin(yaw);
        auto cosYaw = std::cos(yaw);
        auto step = moveSpeed * delta;

        moveWithSliding((-sinYaw * forward + cosYaw * strafe) * step,
                        (-cosYaw * forward - sinYaw * strafe) * step);
    }

    // Axes resolve separately so a blocked diagonal slides along the wall
    // instead of sticking to it.
    void moveWithSliding(float dx, float dz)
    {
        if (canStandAt(posX + dx, posZ))
            posX += dx;

        if (canStandAt(posX, posZ + dz))
            posZ += dz;
    }

    static bool canStandAt(float x, float z)
    {
        constexpr auto radius = 0.25f;

        for (auto cornerX: {x - radius, x + radius})
            for (auto cornerZ: {z - radius, z + radius})
                if (isSolid((int) std::floor(cornerX), (int) std::floor(cornerZ)))
                    return false;

        return true;
    }

    void render(Frame& frame) override
    {
        auto bounds = getLocalBounds();

        shader.camX = posX;
        shader.camZ = posZ;
        shader.yaw = yaw;
        shader.aspect = bounds.h > 0.0f ? bounds.w / bounds.h : 1.0f;

        auto pass = frame.beginPass({Graphics::Color {0.0f, 0.0f, 0.0f}});
        pass.draw(shader);
    }

    static constexpr float moveSpeed = 3.0f;
    static constexpr float turnSpeed = 2.4f;
    static constexpr float mouseSensitivity = 0.0035f;

    Graphics::Window* window = nullptr;
    MazeMesh mesh;
    Texture atlas;
    MazeShader shader;

    float posX = 1.5f;
    float posZ = 1.5f;
    float yaw = -pi / 2.0f;
};

struct MyApp
{
    MyApp()
    {
        window.setContentView(maze);
        maze.window = &window;
        maze.focus();
    }

    MazeView maze;
    Graphics::Window window;
};

int main()
{
    return eacp::Apps::run<MyApp>();
}
