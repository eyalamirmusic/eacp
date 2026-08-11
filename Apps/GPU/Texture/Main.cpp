#include <eacp/GPU/GPU.h>

using namespace eacp;
using namespace GPU;

struct Vertex
{
    float position[2];
    float uv[2];
};

namespace
{
// v runs 0 -> 1 from the quad's top edge down, so texture row 0 is at the top.
constexpr Vertex quad[] = {
    {{-0.8f, -0.8f}, {0.0f, 1.0f}},
    {{0.8f, -0.8f}, {1.0f, 1.0f}},
    {{0.8f, 0.8f}, {1.0f, 0.0f}},
    {{-0.8f, -0.8f}, {0.0f, 1.0f}},
    {{0.8f, 0.8f}, {1.0f, 0.0f}},
    {{-0.8f, 0.8f}, {0.0f, 0.0f}},
};

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

    return device.makeTexture(descriptor, pixels);
}

struct TexturedShader final : ShaderProgram
{
    TexturedShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&Vertex::position);
        auto uv = vertexInput(&Vertex::uv);

        setPosition(float4(position, 0.0f, 1.0f));
        setFragment(sample(image, varying(uv)));
    }

    Uniform<Texture2D> image;

    EACP_SHADER(image)
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
    return eacp::Apps::run<MyApp>();
}
