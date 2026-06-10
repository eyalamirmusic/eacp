#include <eacp/GPU/GPU.h>
#include <eacp/Graphics/Graphics.h>

using namespace eacp;
using namespace GPU;

struct Vertex
{
    float position[2];
    float uv[2];
};

namespace
{
// A full quad as two triangles. Texture coordinates put row 0 at the top, so v
// runs 0 -> 1 from the quad's top edge down.
constexpr Vertex quad[] = {
    {{-0.8f, -0.8f}, {0.0f, 1.0f}},
    {{0.8f, -0.8f}, {1.0f, 1.0f}},
    {{0.8f, 0.8f}, {1.0f, 0.0f}},
    {{-0.8f, -0.8f}, {0.0f, 1.0f}},
    {{0.8f, 0.8f}, {1.0f, 0.0f}},
    {{-0.8f, 0.8f}, {0.0f, 0.0f}},
};

// A procedural checkerboard, sampled with nearest filtering so the cells stay
// crisp however large the quad is drawn.
Texture makeCheckerboard(Device& device)
{
    constexpr auto size = 8;
    std::uint32_t pixels[size * size];

    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            pixels[y * size + x] = (x + y) % 2 == 0 ? 0xff3a3f4b : 0xffe8ecf2;

    auto descriptor = TextureDescriptor {};
    descriptor.width = size;
    descriptor.height = size;
    descriptor.filter = TextureFilter::Nearest;

    return device.makeTexture(descriptor, pixels);
}

// The textured quad authored as a struct: the texture is a member you assign a
// GPU::Texture to, and the fragment colour is a sample of it at the
// interpolated vertex UV.
struct TexturedShader final : ShaderProgram
{
    Uniform<Texture2D> image;

    EACP_SHADER(image)

    TexturedShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&Vertex::position);
        auto uv = vertexInput(&Vertex::uv);

        setPosition(float4(position, 0.0f, 1.0f));
        setFragment(sample(image, varying(uv)));
    }
};
} // namespace

struct TextureView final : GPUView
{
    TextureView()
        : checkerboard(makeCheckerboard(Device::shared()))
    {
        shader.setVertices(quad);
        shader.prepare(sampleCount());
        shader.image = checkerboard;
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({Graphics::Color {0.09f, 0.10f, 0.13f}});
        pass.draw(shader);
    }

    Texture checkerboard;
    TexturedShader shader;
};

struct MyApp
{
    MyApp() { window.setContentView(view); }

    TextureView view;
    Graphics::Window window;
};

int main()
{
    eacp::Apps::run<MyApp>();
    return 0;
}
