#pragma once

/** @file ShaderProgram, a struct-authored render shader that owns its vertex buffer,
    library and pipeline.

    A shader authored as a struct. Uniforms are named, typed members you set by
    name; vertex inputs are pulled straight out of the CPU vertex struct inside
    define(), so that struct is the single source of the vertex layout. The program
    also owns its realized GPU resources (vertex buffer, library, pipeline), so a
    view feeds data and draws without juggling loose handles.

      struct Vertex { Maths::Vec2 position; Maths::Vec3 color; };

      struct MyShader final : ShaderProgram
      {
          Uniform<Float> angle;
          EACP_SHADER(angle)

          MyShader() { compile(); }

          void define() override
          {
              auto position = vertexInput(&Vertex::position);   // -> Float2
              auto color    = vertexInput(&Vertex::color);      // -> Float3
              auto c = cos(angle);
              auto s = sin(angle);
              auto rotated = float2(position.x() * c - position.y() * s,
                                    position.x() * s + position.y() * c);
              setPosition(float4(rotated, 0.0f, 1.0f));
              setFragment(float4(varying(color), 1.0f));
          }
      };

      struct TriangleView final : GPUView
      {
          TriangleView()
          {
              shader.setVertices(vertices);       // typed; owns the buffer
              shader.prepare(sampleCount());      // builds library + pipeline
              setContinuous(true);
          }

          void update(Threads::FrameTime time) override
          {
              angle += static_cast<float>(time.delta);
          }

          void render(Frame& frame) override
          {
              shader.angle = angle;
              auto pass = frame.beginPass({});
              pass.draw(shader);                  // pipeline + vertices + uniforms
          }

          static inline const Vertex vertices[] = {
              {{0.0f, 0.8f}, {1.0f, 0.2f, 0.2f}},
              {{-0.8f, -0.8f}, {0.2f, 1.0f, 0.2f}},
              {{0.8f, -0.8f}, {0.2f, 0.2f, 1.0f}},
          };

          MyShader shader;
          float angle = 0.0f;
      };

    Instancing follows the same shape: pull per-instance fields with
    instanceInput(&Instance::field, slot), feed them with setInstances(slot, ...),
    and draw with pass.drawInstanced(shader, instanceCount [, firstInstance]). The
    per-vertex geometry stays at slot 0; each instanceInput slot becomes a
    PerInstance vertex-buffer binding.

          auto pos    = vertexInput(&Vertex::position);       // slot 0, per vertex
          auto centre = instanceInput(&Instance::centre, 1);  // slot 1, per instance
*/

#include <eacp/Core/Utils/Containers.h>

#include <array>
#include <iterator>

#include "../Buffer/StreamingBuffers.h"
#include "../Device/Device.h"
#include "../Frame/RenderPass.h"
#include "Forward.h"
#include "GeneratedShader.h"
#include "ShaderBuilder.h"
#include "ShaderMembers.h"
#include "ShaderTypes.h"
#include "ShaderValue.h"

// A shader authored as a struct. Uniforms are named, typed members you set by
// name; vertex inputs are pulled straight out of the CPU vertex struct inside
// define(), so that struct is the single source of the vertex layout. The program
// also owns its realized GPU resources (vertex buffer, library, pipeline), so a
// view feeds data and draws without juggling loose handles.
//
//   struct Vertex { float position[2]; float color[3]; };
//
//   struct MyShader final : ShaderProgram
//   {
//       Uniform<Float> angle;
//       EACP_SHADER(angle)
//
//       MyShader() { compile(); }
//
//       void define() override
//       {
//           auto position = vertexInput(&Vertex::position);   // -> Float2
//           auto color    = vertexInput(&Vertex::color);      // -> Float3
//           setPosition(float4(position, 0.0f, 1.0f));
//           setFragment(float4(varying(color), 1.0f));
//       }
//   };
//
//   MyShader shader;
//   shader.setVertices(triangleVertices);   // typed; owns the buffer
//   shader.prepare(view.sampleCount());     // builds library + pipeline
//   ...
//   shader.angle = 0.5f;
//   pass.draw(shader);                       // pipeline + vertices + uniforms
//
// Instancing follows the same shape: pull per-instance fields with
// instanceInput(&Instance::field, slot), feed them with setInstances(slot, ...),
// and draw with pass.drawInstanced(shader, instanceCount [, firstInstance]). The
// per-vertex geometry stays at slot 0; each instanceInput slot becomes a
// PerInstance vertex-buffer binding.
//
//       auto pos    = vertexInput(&Vertex::position);       // slot 0, per vertex
//       auto centre = instanceInput(&Instance::centre, 1);  // slot 1, per instance

namespace eacp::GPU
{
namespace detail
{
// Whether a texture is what a storage member asks for: valid, in the member's
// format, and of its shape - a 2D array taking a cube or a cube array as the
// array of its faces.
bool fitsStorageMember(const Texture& texture, const StorageTextureLayout& layout);

// Whether a sampler member binds the sampler assigned to it: a valid one, with
// a CompareFunction exactly where the member is a comparison sampler. A
// mismatch is logged, once per process, naming the member.
bool fitsSamplerMember(const char* name, const Sampler& sampler, bool comparison);

bool isCubeType(TextureType type);
} // namespace detail

constexpr VertexFormat toVertexFormat(ValueType type)
{
    switch (type)
    {
        case ValueType::Float:
            return VertexFormat::Float;
        case ValueType::Float2:
            return VertexFormat::Float2;
        case ValueType::Float3:
            return VertexFormat::Float3;
        default:
            return VertexFormat::Float4; // matrix/integer/bool are never attributes
    }
}

// A CPU vertex field's wire format.
//
// The default is whatever its shader value implies, which is the unpacked one -
// a Float4 attribute is four floats. A packed type overrides it by declaring a
// vertexFormat of its own, which is the whole mechanism: the field keeps saying
// what the shader sees through ShaderValue, and says separately what the buffer
// holds. See PackedVertex.h.
template <typename T>
struct VertexFormatOf
{
    static constexpr auto value =
        toVertexFormat(ValueTypeOf<typename ShaderValueOf<T>::type>::value);
};

template <typename T>
    requires requires { T::vertexFormat; }
struct VertexFormatOf<T>
{
    static constexpr auto value = T::vertexFormat;
};

// What a field of type M is expected to occupy, for the size check every input
// makes. A packed field is measured against the format it declares; anything
// else against the CPU type its shader value implies - the same question asked
// of whichever of the two is authoritative for that field.
template <typename M, typename Handle>
constexpr int expectedAttributeBytes()
{
    if constexpr (requires { M::vertexFormat; })
        return bytesPerAttribute(M::vertexFormat);
    else
        return (int) sizeof(typename CpuValueOf<Handle>::type);
}

// Base for struct-authored shaders. Derive, declare uniform members, list them
// with EACP_SHADER, write define() (pulling vertex inputs from the CPU vertex
// struct), and call compile() from the constructor.
class ShaderProgram
{
public:
    ShaderProgram();
    virtual ~ShaderProgram();

    // Uniform members and pulled vertex handles point into the owned builder's
    // graph, and the owned GPU resources are non-copyable, so a program is pinned
    // in place.
    ShaderProgram(const ShaderProgram&) = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;

    const ShaderSource& source() const;

    const VertexLayout& vertexLayout() const;

    // The shader as the EDSL recorded it; source() is one platform's spelling.
    const ShaderGraph& graph() const;

    // What the program binds and passes - each member at its slot and under
    // its name with the stages that read it, the uniform block's members at
    // their offsets, the vertex inputs and varyings, overrides, functions and
    // structs - read from graph() (ShaderReflection.h).
    const ShaderReflection& reflection() const;

    // Uploads the typed vertex data and owns the resulting buffer. The element
    // type's size must match the layout pulled from it in define().
    template <typename V, std::size_t N>
    void setVertices(const V (&data)[N])
    {
        setVertices(data, (int) N);
    }

    template <typename Range>
        requires requires(const Range& range) {
            std::data(range);
            std::size(range);
        }
    void setVertices(const Range& data)
    {
        setVertices(std::data(data), (int) std::size(data));
    }

    template <typename V>
    void setVertices(const V* data, int count)
    {
        assert((int) sizeof(V) == vertexLayout().stride
               && "vertex element size does not match the shader's vertex layout");

        // Widened before the multiply rather than after it: the product is the
        // buffer's byte count, and a large enough mesh overflows an int on the
        // way in, where the parameter it lands in would have held it.
        vertexBufferData.emplace(
            Device::shared(), data, (std::int64_t) sizeof(V) * count);
        vertexCountValue = count;
    }

    // Uploads typed index data and owns the resulting buffer; draw(program)
    // then draws indexed. The index width is inferred from the element type.
    template <std::size_t N>
    void setIndices(const std::uint32_t (&data)[N])
    {
        setIndices(data, (int) N);
    }

    template <std::size_t N>
    void setIndices(const std::uint16_t (&data)[N])
    {
        setIndices(data, (int) N);
    }

    void setIndices(const std::uint32_t* data, int count);
    void setIndices(const std::uint16_t* data, int count);

    // Uploads typed per-instance data for a buffer slot and owns the storage.
    // bufferIndex must match the slot an instanceInput() pulled into; the
    // element type's size must match that slot's per-instance stride. All
    // instance slots carry the same element count - the instance count passed
    // to RenderPass::drawInstanced(program, ...).
    //
    // Each call gets storage no earlier call's draw is still reading, so one
    // program can be flushed many times in a frame - which SpriteRenderer and
    // Text::GlyphRenderer both do, on every texture, sampling and scissor
    // change. StreamingBuffers hands
    // each call its own slice of one arena per frame in flight, and so
    // allocates nothing once its pools are warm.
    template <typename I, std::size_t N>
    void setInstances(int bufferIndex, const I (&data)[N])
    {
        setInstances(bufferIndex, data, (int) N);
    }

    template <typename I>
    void setInstances(int bufferIndex, const I* data, int count)
    {
        assert(bufferIndex >= 0 && bufferIndex < vertexLayout().buffers.size()
               && "instance buffer slot was not declared via instanceInput");
        assert((int) sizeof(I) == vertexLayout().buffers[bufferIndex].stride
               && "instance element size does not match the shader's "
                  "per-instance layout");

        if (instanceBuffers.size() <= bufferIndex)
        {
            instanceStreams.resize(bufferIndex + 1);
            instanceBuffers.resize(bufferIndex + 1);
        }

        auto& stream = instanceStreams[bufferIndex];

        if (!stream.has_value())
            stream.emplace(BufferUsage::Vertex);

        // Widened before the multiply, as setVertices is and for the reason it
        // is: the product is a byte count.
        instanceBuffers[bufferIndex] =
            stream->write(data, (std::int64_t) sizeof(I) * count);
        instanceCountValue = count;
        setExternalInstanceBuffer(bufferIndex, nullptr);
    }

    // Points an instance slot at a buffer the program does not own — the
    // compute path's counterpart to setInstances, whose data comes from the CPU.
    //
    // A kernel writing per-particle state into a Storage buffer and a draw
    // reading that same buffer as its per-instance stream is what
    // Frame::beginCompute exists for: the data is produced and consumed on the
    // GPU, and the CPU never sees a byte of it. The buffer must outlive the
    // draw, and its elements must match the per-instance stride that
    // instanceInput() declared for this slot.
    void setInstanceBuffer(int bufferIndex, const Buffer& buffer, int count);

    // Builds the shader library and render pipeline. sampleCount must match the
    // render target (GPUView::sampleCount()); set depth when the view has a depth
    // buffer (GPUView::setDepth(true)).
    //
    // blendMode defaults to None, which writes fragments straight through. A
    // program drawing anything translucent — glyph coverage, a fade, a scrim —
    // needs AlphaBlend, or its antialiased edges punch holes in what is behind
    // them instead of blending with it. Without this, such a program had to
    // build its pipeline by hand and give up draw(program) entirely.
    //
    // colorFormat is the attachment the pipeline writes, and it has to be the
    // format of whatever the draw ends up in: the default is a view's drawable,
    // and a program rendering into a texture passes pixelFormatFor(its format)
    // instead. Neither backend will take a draw whose pipeline disagrees with
    // its attachment.
    void prepare(int sampleCount,
                 bool depth = false,
                 PrimitiveTopology topology = PrimitiveTopology::Triangles,
                 BlendMode blendMode = BlendMode::None,
                 PixelFormat colorFormat = PixelFormat::BGRA8Unorm);

    // The named form of the same thing, and what to reach for once more than
    // one of these is not the shader's own choice: a program drawing into a
    // texture takes its sample count, its depth and its pixel format from the
    // target, and four positional arguments in a row say none of that at the
    // call site. Cull mode, front face and the depth comparison are only
    // reachable this way, having no positional slots.
    //
    // The program's own library and vertex layout are what they always were and
    // are filled in here; whatever the caller left in those two fields is
    // ignored.
    void prepare(RenderPipelineDescriptor descriptor);

    // The same, with values for the shader's overrides (OverrideValues.h),
    // both stages alike; the ones left out take their defaults. Values the
    // shader refuses leave a pipeline that is not valid, and log why. The
    // descriptor's own constants are replaced by these. Every prepare()
    // compiles the library afresh and forgets the pipelines built before it.
    void prepare(RenderPipelineDescriptor descriptor,
                 const OverrideValues& overrides);

    // Makes the shader specialised with these values the one draw(program)
    // uses, on the library and the descriptor the last prepare() was given:
    // the pipeline already built for the same values since then, or one built
    // now. See ComputeProgram::specialize. Returns whether it is valid.
    bool specialize(const OverrideValues& overrides);

    // Whether the device runs this shader's f16 values, if it has any
    // (Device::supportsShaderFloat16). prepare() refuses one that does not
    // fit, leaving a pipeline that is not valid and logging why.
    bool fitsShaderFloat16(const Device& device) const;

    // Whether the device runs this shader's fragment stage per sample, if it
    // asks for that - a sampleIndex() read or a Sample-interpolated varying
    // (Device::supportsSampleRateShading). prepare() refuses one that does not
    // fit, as it does an f16 one.
    bool fitsSampleRateShading(const Device& device) const;

    const RenderPipeline& pipeline() const;

    // Whether prepare() loaded the shader from a blob compiled at build time
    // rather than compiling its source. False before prepare().
    bool isPrecompiled() const;

    const Buffer& vertices() const;
    constexpr int vertexCount() const { return vertexCountValue; }

    // Whether setVertices ever gave this program geometry of its own. A program
    // only ever drawn through RenderPass::bind(program, vertices) has none, and
    // asking it for vertices() would dereference an empty optional - which is
    // why that overload does not.
    bool hasVertices() const;

    bool hasIndices() const;
    const Buffer& indices() const;
    constexpr int indexCount() const { return indexCountValue; }
    constexpr IndexFormat indexFormat() const { return indexFormatValue; }

    // Re-packs the current uniform values and returns the block, ready for
    // RenderPass::setVertexBytes. Cheap - the block is a handful of floats.
    const void* packedUniforms();

    int uniformByteSize() const;
    bool hasUniforms() const;

    // Which stage define() actually read a uniform from, answered by the same
    // walk that decided whether to declare the block in that stage's generated
    // function. RenderPass::draw(program) binds to the stage that says yes and
    // leaves the other alone; a program declaring uniforms neither stage reads
    // binds to nobody. Ask these rather than hasUniforms() when hand-rolling a
    // draw over app-owned geometry.
    bool vertexReadsUniforms() const;
    bool fragmentReadsUniforms() const;

    // Binds every assigned texture member to the pass - to the fragment stage,
    // and to the vertex stage as well where vertexStage() reads it; a no-op for
    // programs without textures. RenderPass::bind and RenderPass::draw(program) call it
    // once - and so does a caller drawing sub-ranges of one buffer that differ
    // by their texture, which is what this is public for: after bind(), the
    // per-draw state is the caller's to restate, and a texture is the commonest
    // thing it is.
    void bindTextures(RenderPass& pass);

    // Its storage-buffer sibling: binds every assigned Uniform<InputBuffer> so
    // define() can subscript it at an index the shader computed - reading a
    // record a kernel produced, rather than receiving it as an attribute. A
    // no-op for programs without buffers. RenderPass::draw(program) calls this.
    void bindBuffers(RenderPass& pass);

    // True once any instanceInput() was pulled: the program feeds one or more
    // per-instance buffers and is drawn with drawInstanced(program, ...).
    constexpr bool isInstanced() const { return usesInstancing; }

    // The element count last uploaded via setInstances - the number of
    // instances the owned per-instance buffers hold.
    constexpr int instanceCount() const { return instanceCountValue; }

    // Binds every per-instance buffer at the slot it was given to, whether the
    // program uploaded it or a kernel filled it. RenderPass::drawInstanced(
    // program, ...) calls this after binding the per-vertex buffer at slot 0.
    void bindInstances(RenderPass& pass);

protected:
    // Runs the uniform build walk, the user's define() (which pulls vertex inputs),
    // then emits source + layouts. Called from the most-derived constructor.
    void compile();

    // Pulls a vertex attribute out of the CPU vertex struct. The field's type maps
    // to a shader value via ShaderValueOf, the attribute takes the field's real
    // offset, and the returned handle is what you write the shader with.
    template <typename C, typename M>
    typename ShaderValueOf<M>::type vertexInput(M C::* member)
    {
        using Handle = typename ShaderValueOf<M>::type;
        static_assert((int) sizeof(M) == expectedAttributeBytes<M, Handle>(),
                      "vertex field size does not match the format it declares");

        constexpr auto type = ValueTypeOf<Handle>::value;
        vertexLayoutData.attribute(VertexFormatOf<M>::value, memberOffset(member));
        vertexLayoutData.stride = (int) sizeof(C);

        auto added = builder.addVertexInput(type);

        auto handle = Handle {};
        handle.graph = added.graph;
        handle.node = added.node;
        return handle;
    }

    // Per-instance sibling of vertexInput: pulls an attribute out of a CPU
    // per-instance struct and routes it to a dedicated buffer slot with
    // PerInstance step rate. bufferIndex is the slot the matching per-instance
    // buffer binds to (setInstances(bufferIndex, ...)); the per-vertex geometry
    // always stays at slot 0. Use 1 for a single per-instance stream, and
    // distinct slots (1, 2, ...) when a shader needs several - e.g. a transform
    // stream in slot 1 and a colour stream in slot 2.
    template <typename C, typename M>
    typename ShaderValueOf<M>::type instanceInput(M C::* member, int bufferIndex)
    {
        using Handle = typename ShaderValueOf<M>::type;
        static_assert((int) sizeof(M) == expectedAttributeBytes<M, Handle>(),
                      "instance field size does not match the format it declares");

        constexpr auto type = ValueTypeOf<Handle>::value;
        vertexLayoutData.attribute(
            VertexFormatOf<M>::value, memberOffset(member), bufferIndex);
        vertexLayoutData.buffer(bufferIndex, (int) sizeof(C), StepRate::PerInstance);
        usesInstancing = true;

        auto added = builder.addInstanceInput(type, bufferIndex);

        auto handle = Handle {};
        handle.graph = added.graph;
        handle.node = added.node;
        return handle;
    }

    Float varying(const Float& vertexValue);
    Float2 varying(const Float2& vertexValue);
    Float3 varying(const Float3& vertexValue);
    Float4 varying(const Float4& vertexValue);
    UInt varying(const UInt& vertexValue);

    // With WGSL's @interpolate; see ShaderBuilder::varying.
    Float varying(const Float& vertexValue, const Interpolation& interpolation);
    Float2 varying(const Float2& vertexValue, const Interpolation& interpolation);
    Float3 varying(const Float3& vertexValue, const Interpolation& interpolation);
    Float4 varying(const Float4& vertexValue, const Interpolation& interpolation);
    UInt varying(const UInt& vertexValue, const Interpolation& interpolation);

    // The stage built-ins; see ShaderBuilder for what each one holds.
    Float4 fragmentCoordinate();
    UInt vertexId();
    UInt instanceId();
    Bool frontFacing();
    UInt sampleIndex();
    UInt sampleMask();

    Float constant(float value);
    Half halfConstant(float value);
    Bool boolean(bool value);
    Int integer(int value);
    UInt unsignedInteger(unsigned value);

    // WGSL's override, its value given at prepare() (OverrideValues.h) and
    // readable in both stages. See ShaderBuilder::overrideConstant.
    template <OverridableHandle T>
    T overrideConstant(const std::string& name)
    {
        return builder.overrideConstant<T>(name);
    }

    template <OverridableHandle T>
    T overrideConstant(const std::string& name,
                       typename detail::OverrideDefault<T>::type defaultValue)
    {
        return builder.overrideConstant<T>(name, defaultValue);
    }

    template <ShaderValueLike T, SameShaderShape<T>... Rest>
    ConstantArray<ShaderBase<T>, 1 + (int) sizeof...(Rest)>
        array(const T& first, const Rest&... rest)
    {
        return builder.array(first, rest...);
    }

    // Control flow, forwarded from the builder: a mutable local, the two
    // branching statements, the loop and its two jumps. See ShaderBuilder for
    // what each records and why a loop condition is re-tested rather than
    // hoisted.
    // Constrained on the handle rather than on the float vocabulary, the way
    // the builder's own is: the cell a shader walks a grid with is a variable
    // on the same terms a colour is, and so is the orientation it builds up
    // over several steps - which is the matrix overload beside it, outside all
    // three families for the reason a matrix is outside them everywhere here.
    template <ShaderHandleLike T>
    Var<ShaderHandle<T>> var(const T& initialValue)
    {
        return builder.var(initialValue);
    }

    template <typename T>
        requires(isMatrix(ValueTypeOf<T>::value))
    Var<T> var(const T& initialValue)
    {
        return builder.var(initialValue);
    }

    template <ShaderStructLike T>
    Var<T> var(const T& initialValue)
    {
        return builder.var(initialValue);
    }

    // A shader struct's zero value.
    template <ShaderStructLike T>
    T structZero()
    {
        return builder.structZero<T>();
    }

    Var<Float> var(float initialValue);
    Var<Bool> var(bool initialValue);
    Var<Int> var(int initialValue);
    Var<UInt> var(unsigned initialValue);

    // A private array per invocation, in the stage open when it is created
    // (see PrivateArray and ShaderBuilder::privateArray).
    template <typename T, int Size>
    PrivateArray<T, Size> privateArray()
    {
        return builder.privateArray<T, Size>();
    }

    template <typename T, typename... Rest>
        requires(ShaderHandleLike<T> || ShaderMatrixLike<T>)
    PrivateArray<detail::StoredHandle<T>, 1 + (int) sizeof...(Rest)>
        privateArray(const T& first, const Rest&... rest)
    {
        return builder.privateArray(first, rest...);
    }

    template <typename Body>
    void ifThen(const Bool& condition, Body&& body)
    {
        builder.ifThen(condition, std::forward<Body>(body));
    }

    template <typename Then, typename Else>
    void ifThen(const Bool& condition, Then&& whenTrue, Else&& whenFalse)
    {
        builder.ifThen(
            condition, std::forward<Then>(whenTrue), std::forward<Else>(whenFalse));
    }

    template <typename Body>
    void loop(const Bool& condition, Body&& body)
    {
        builder.loop(condition, std::forward<Body>(body));
    }

    template <typename Body, typename Continuing>
    void loop(const Bool& condition, Body&& body, Continuing&& continuing)
    {
        builder.loop(condition,
                     std::forward<Body>(body),
                     std::forward<Continuing>(continuing));
    }

    template <typename Update, typename Body>
    void forLoop(const Bool& condition, Update&& update, Body&& body)
    {
        builder.forLoop(
            condition, std::forward<Update>(update), std::forward<Body>(body));
    }

    void breakLoop();
    void continueLoop();
    void breakLoopIf(const Bool& condition);

    template <typename Selector, typename... Bodies>
    void switchOn(const Selector& selector, const SwitchClause<Bodies>&... clauses)
    {
        builder.switchOn(selector, clauses...);
    }

    template <typename Body>
    static SwitchClause<std::decay_t<Body>> caseOf(std::int64_t value, Body&& body)
    {
        return GPU::caseOf(value, std::forward<Body>(body));
    }

    template <typename Body>
    static SwitchClause<std::decay_t<Body>>
        caseOf(std::initializer_list<std::int64_t> values, Body&& body)
    {
        return GPU::caseOf(values, std::forward<Body>(body));
    }

    template <typename Body>
    static SwitchClause<std::decay_t<Body>> defaultCase(Body&& body)
    {
        return GPU::defaultCase(std::forward<Body>(body));
    }

    // The fragment or vertex function returns here with its outputs as they
    // evaluate now, and discard() drops the fragment. See ShaderBuilder.
    // Inside a function's body they are the function's: returnEarly() leaves
    // it, and returnEarly(value) returns its result.
    void returnEarly();
    void discard();

    template <ShaderFunctionValue T>
    void returnEarly(const T& value)
    {
        builder.returnEarly(value);
    }

    // WGSL's fn: a function emitted once and called from either stage through
    // the handle returned. See ShaderBuilder::function and ShaderFunction.h.
    template <typename Signature, typename Body>
    ShaderFunction<Signature> function(const std::string& name, Body&& body)
    {
        return builder.function<Signature>(name, std::forward<Body>(body));
    }

    // The statements `body` records go to the vertex function. See
    // ShaderBuilder::vertexStage.
    template <typename Body>
    void vertexStage(Body&& body)
    {
        builder.vertexStage(std::forward<Body>(body));
    }

    // In-shader transform builders, matching column-major / right-handed [0,1]
    // depth conventions. Build the model/view/projection inside define() from
    // scalar uniforms instead of uploading prebuilt matrices.
    Float4x4 translate(float x, float y, float z);
    Float4x4 translate(const Float& x, const Float& y, const Float& z);
    Float4x4 rotateY(const Float& angle);
    Float4x4 rotateZ(const Float& angle);
    Float4x4 rotateX(float radians);

    // aspect is a live uniform; the field of view, near and far are baked in.
    Float4x4 perspective(const Float& aspect, float fovY, float nearZ, float farZ);

    void setPosition(const Float4& clipPosition);
    void setFragment(const Float4& color);

    // The fragment's other two outputs, beside its colour: a depth replacing
    // the rasterized one, and a coverage mask over the samples it writes. See
    // ShaderBuilder::fragmentDepth and fragmentSampleMask.
    void setFragmentDepth(const Float& depth);
    void setSampleMask(const UInt& mask);

    // Kills the fragment when value falls below threshold, before any colour or
    // depth is written — the alpha test a masked texture needs, so a sprite's
    // transparent pixels or the holes in a grate leave what is behind them
    // visible instead of occluding it.
    void setDiscardBelow(const Float& value, float threshold);

    // Generated by EACP_SHADER: visits each declared uniform member in order.
    virtual void reflectMembers(ShaderVisitor& visitor) = 0;

    // Written by the user: the shader body, pulling vertex inputs as needed.
    virtual void define() = 0;

private:
    void packUniforms();

    void setExternalInstanceBuffer(int bufferIndex, const Buffer* buffer);

    // A slot carries either an owned upload or a borrowed buffer; the last call
    // for that slot wins, so a program can be re-pointed between the two. A
    // borrowed buffer is bound whole, from its start; an owned upload is the
    // slice of the stream's arena that setInstances was given. A slot holding
    // neither comes back with a null buffer.
    BufferRange instanceBufferAt(int slot) const;

    // A buffer per call, deliberately, and not something to "optimise" into
    // reuse. A program is routinely drawn more than once in a frame with
    // different data -- that is what a batching renderer is -- and both draws
    // read their buffer when the frame executes, not when it was encoded.
    // Refilling one buffer in place would hand the first draw the second's
    // instances: on Metal because update() is a CPU memcpy into shared memory,
    // and on D3D12 because the copy would land on the queue after the draw that
    // wanted the old contents.
    //
    // Replacing it is what keeps both correct: the encoder retains the old
    // resource (deferRelease on D3D12, the command encoder on Metal), so the
    // first draw keeps reading the bytes it was given. Making that cheap is the
    // backend's job -- see Buffer-Windows.cpp.
    void uploadIndices(const void* data,
                       int elementSize,
                       int count,
                       IndexFormat format);

    ShaderBuilder builder;
    GeneratedShader generated;
    VertexLayout vertexLayoutData;
    Vector<std::byte> uniformBytes;

    // An invalid pipeline made current, for a refused shader or values.
    void useRefusedPipeline();

    // One pipeline per set of constants the shader was specialised with.
    struct Specialization
    {
        explicit Specialization(const RenderPipelineDescriptor& descriptor)
            : constants(descriptor.constants)
            , pipeline(Device::shared(), descriptor)
        {
        }

        PipelineConstants constants;
        RenderPipeline pipeline;
    };

    std::optional<Buffer> vertexBufferData;
    std::optional<Buffer> indexBufferData;
    std::optional<ShaderLibrary> shaderLibrary;
    std::optional<RenderPipelineDescriptor> preparedDescriptor;
    OwnedVector<Specialization> specializations;
    std::optional<ShaderLibrary> refusedLibrary;
    std::optional<RenderPipeline> refusedPipeline;
    const RenderPipeline* currentPipeline = nullptr;

    // Per-instance buffers indexed by their vertex-buffer slot; slot 0 stays
    // empty (the per-vertex buffer). Populated by setInstances, bound by
    // bindInstances. usesInstancing gates the multi-slot layout in compile().
    //
    // The stream owns a slot's storage across frames; the range beside it is
    // the slice the last setInstances wrote, which is what a bind needs. Two
    // of them rather than one because the stream hands out a different slice
    // each call, and the slot has to remember which.
    bool usesInstancing = false;
    Vector<std::optional<StreamingBuffers>> instanceStreams;
    Vector<BufferRange> instanceBuffers;

    // Slots pointed at a buffer someone else owns (setInstanceBuffer), which is
    // how a compute kernel's output is drawn. Parallel to instanceBuffers so a
    // slot can be moved between an upload and a kernel's output.
    Vector<const Buffer*> externalInstanceBuffers;
    int instanceCountValue = 0;

    int vertexCountValue = 0;
    int indexCountValue = 0;
    IndexFormat indexFormatValue = IndexFormat::UInt32;
};
} // namespace eacp::GPU
