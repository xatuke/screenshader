// screenshader-macos.swift
// Single-file macOS screen shader overlay using ScreenCaptureKit + Metal
//
// Usage:
//   screenshader-macos [shader.frag]     Start with shader
//   screenshader-macos --stop            Stop (kill by PID)
//   screenshader-macos --list            List available shaders
//
// Requires: macOS 14+, Screen Recording permission

import Cocoa
import Metal
import MetalKit
import ScreenCaptureKit
import CoreMedia

// MARK: - GLSL → MSL Converter

/// Converts our limited GLSL 330 core fragment shaders to Metal Shading Language.
/// Handles the specific subset used by screenshaders: texture sampling, uniforms,
/// helper functions, and main() body.
func convertGLSLtoMSL(_ glsl: String) -> String {
    var lines = glsl.components(separatedBy: "\n")

    // Collect custom uniforms (beyond the standard three + u_screen)
    var customUniforms: [(type: String, name: String)] = []
    for line in lines {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        if trimmed.hasPrefix("uniform float ") && !trimmed.contains("u_time") {
            let name = trimmed
                .replacingOccurrences(of: "uniform float ", with: "")
                .replacingOccurrences(of: ";", with: "")
                .trimmingCharacters(in: .whitespaces)
            customUniforms.append((type: "float", name: name))
        }
    }

    // Remove GLSL-specific lines
    lines = lines.filter { line in
        let t = line.trimmingCharacters(in: .whitespaces)
        if t.hasPrefix("#version") { return false }
        if t == "in vec2 v_texcoord;" { return false }
        if t == "out vec4 frag_color;" { return false }
        if t.hasPrefix("uniform ") { return false }
        return true
    }

    var body = lines.joined(separator: "\n")

    // Type renames
    body = body.replacingOccurrences(of: "vec2", with: "float2")
    body = body.replacingOccurrences(of: "vec3", with: "float3")
    body = body.replacingOccurrences(of: "vec4", with: "float4")
    body = body.replacingOccurrences(of: "mat2", with: "float2x2")
    body = body.replacingOccurrences(of: "ivec2", with: "int2")
    body = body.replacingOccurrences(of: "ivec3", with: "int3")
    body = body.replacingOccurrences(of: "sampler2D", with: "texture2d<float>")

    // Function renames
    body = body.replacingOccurrences(of: "fract(", with: "fract(")  // same in MSL
    body = body.replacingOccurrences(of: "mix(", with: "mix(")      // same in MSL

    // texture() → u_screen.sample(samp, ...)
    // Pattern: texture(u_screen, expr) → u_screen.sample(samp, expr)
    body = convertTextureCalls(body)

    // mod(a, b) → fmod(a, b)
    body = body.replacingOccurrences(of: "mod(", with: "fmod(")

    // inverse(m) → Metal doesn't have inverse() for float2x2, provide helper
    let needsInverse = body.contains("inverse(")

    // frag_color = ... → return ...
    // We handle this in the main() extraction

    // const float → constant float (top-level only — inside functions, const is fine in Metal)
    // Convert top-level const declarations before extractMainBody splits things.
    do {
        var convertedLines: [String] = []
        var braceDepth = 0
        for line in body.components(separatedBy: "\n") {
            var outLine = line
            if braceDepth == 0 {
                let t = line.trimmingCharacters(in: .whitespaces)
                if t.hasPrefix("const ") {
                    outLine = line.replacingOccurrences(of: "const ", with: "constant ")
                }
            }
            braceDepth += line.filter({ $0 == "{" }).count - line.filter({ $0 == "}" }).count
            convertedLines.append(outLine)
        }
        body = convertedLines.joined(separator: "\n")
    }

    // Build the MSL output
    var msl = """
    #include <metal_stdlib>
    using namespace metal;

    struct VertexOut {
        float4 position [[position]];
        float2 texcoord;
    };

    struct Uniforms {
        float2 resolution;
        float time;

    """

    for u in customUniforms {
        msl += "    \(u.type) \(u.name);\n"
    }

    msl += "};\n\n"

    // Add inverse helper if needed
    if needsInverse {
        msl += """
        float2x2 inverse(float2x2 m) {
            float det = m[0][0] * m[1][1] - m[0][1] * m[1][0];
            float invDet = 1.0 / det;
            return float2x2(
                float2( m[1][1] * invDet, -m[0][1] * invDet),
                float2(-m[1][0] * invDet,  m[0][0] * invDet)
            );
        }

        """
    }

    // Split body into helper functions and main()
    let (helpers, mainBody) = extractMainBody(body)

    // Add helper functions — rewrite their signatures for Metal
    // Helper functions that reference globals (u_screen, u_resolution, u_time)
    // get those added as parameters since they're locals in Metal.
    let helperResult = convertHelperFunctions(helpers)
    msl += helperResult.code

    // Build the fragment function
    msl += """

    fragment float4 postprocess(
        VertexOut in [[stage_in]],
        texture2d<float> u_screen [[texture(0)]],
        sampler samp [[sampler(0)]],
        constant Uniforms& uniforms [[buffer(0)]]
    ) {
        float2 v_texcoord = in.texcoord;
        float2 u_resolution = uniforms.resolution;
        float u_time = uniforms.time;

    """

    for u in customUniforms {
        msl += "    \(u.type) \(u.name) = uniforms.\(u.name);\n"
    }

    // Convert the main body: replace frag_color assignments with returns
    // Also fix call sites of helper functions that got extra params
    var convertedMain = mainBody
    for fixup in helperResult.fixups {
        convertedMain = addExtraArgsToCallSites(convertedMain, funcName: fixup.name, extraArgs: fixup.extraArgs)
    }
    // Handle early returns: frag_color = vec4(...); return; → return float4(...);
    convertedMain = convertedMain.replacingOccurrences(
        of: "frag_color",
        with: "_frag_out"
    )
    // Wrap: declare _frag_out, and at the end return it
    msl += "    float4 _frag_out;\n"
    msl += convertedMain
    msl += "\n    return _frag_out;\n"
    msl += "}\n"

    // Fix any double-return issues: "return;\n" after "_frag_out = ..." should become "return _frag_out;"
    msl = msl.replacingOccurrences(of: "_frag_out = ", with: "_frag_out = ")
    // Replace patterns like: _frag_out = float4(...); return; → return with value
    // Actually let's handle "return;" → "return _frag_out;" inside fragment
    msl = fixEarlyReturns(msl)

    return msl
}

/// Convert texture(sampler, uv) calls to sampler.sample(samp, uv)
func convertTextureCalls(_ code: String) -> String {
    var result = ""
    var i = code.startIndex

    while i < code.endIndex {
        // Look for "texture(" but not "texture2d"
        if code[i...].hasPrefix("texture(") {
            // Find the matching arguments
            let argsStart = code.index(i, offsetBy: 8) // skip "texture("
            if let (texName, uvExpr, endIdx) = parseTextureArgs(code, from: argsStart) {
                result += "\(texName).sample(samp, \(uvExpr))"
                i = endIdx
                continue
            }
        }
        result.append(code[i])
        i = code.index(after: i)
    }

    return result
}

/// Parse texture(name, uvExpr) arguments handling nested parentheses
func parseTextureArgs(_ code: String, from start: String.Index) -> (String, String, String.Index)? {
    // Find the comma separating texture name from UV
    var depth = 0
    var commaIdx: String.Index?
    var idx = start

    while idx < code.endIndex {
        let ch = code[idx]
        if ch == "(" { depth += 1 }
        else if ch == ")" {
            if depth == 0 {
                // End of texture() call
                if let comma = commaIdx {
                    let texName = String(code[start..<comma]).trimmingCharacters(in: .whitespaces)
                    let uvStart = code.index(after: comma)
                    let uvExpr = String(code[uvStart..<idx]).trimmingCharacters(in: .whitespaces)
                    return (texName, uvExpr, code.index(after: idx))
                }
                return nil
            }
            depth -= 1
        } else if ch == "," && depth == 0 && commaIdx == nil {
            commaIdx = idx
        }
        idx = code.index(after: idx)
    }
    return nil
}

/// Extract the main() body from the converted code, separating helper functions
func extractMainBody(_ code: String) -> (helpers: String, mainBody: String) {
    // Find "void main()" and extract its body
    guard let mainRange = code.range(of: "void main()") else {
        return (code, "")
    }

    let helpers = String(code[code.startIndex..<mainRange.lowerBound])

    // Find the opening brace
    var idx = mainRange.upperBound
    while idx < code.endIndex && code[idx] != "{" {
        idx = code.index(after: idx)
    }
    guard idx < code.endIndex else { return (helpers, "") }

    // Find matching closing brace
    var depth = 0
    let bodyStart = code.index(after: idx)
    var bodyEnd = bodyStart

    idx = code.index(after: idx) // skip opening brace
    while idx < code.endIndex {
        if code[idx] == "{" { depth += 1 }
        else if code[idx] == "}" {
            if depth == 0 {
                bodyEnd = idx
                break
            }
            depth -= 1
        }
        idx = code.index(after: idx)
    }

    let mainBody = String(code[bodyStart..<bodyEnd])
    return (helpers, mainBody)
}

/// Convert helper functions — add extra parameters for globals they reference
/// (texture/sampler for u_screen, and u_resolution/u_time as needed)
func convertHelperFunctions(_ helpers: String) -> (code: String, fixups: [(name: String, extraArgs: [String])]) {
    let lines = helpers.components(separatedBy: "\n")
    var output: [String] = []
    var inFunction = false
    var funcLines: [String] = []
    var funcName = ""
    var funcUsesTexture = false
    var funcUsesResolution = false
    var funcUsesTime = false
    var braceDepth = 0

    // Track which functions need extra params so we can fix call sites
    var funcsNeedingTexture: Set<String> = []
    var funcsNeedingResolution: Set<String> = []
    var funcsNeedingTime: Set<String> = []

    for line in lines {
        if !inFunction {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if isFunctionDefinition(trimmed) && !trimmed.hasPrefix("void main") {
                inFunction = true
                funcLines = [line]
                funcName = extractFuncName(trimmed)
                funcUsesTexture = line.contains("u_screen") || line.contains(".sample(samp,")
                funcUsesResolution = line.contains("u_resolution")
                funcUsesTime = line.contains("u_time")
                braceDepth = countBraces(line)
                if braceDepth == 0 && trimmed.hasSuffix("{") {
                    braceDepth = 1
                }
                continue
            }
            output.append(line)
        } else {
            funcLines.append(line)
            if line.contains("u_screen") || line.contains(".sample(samp,") { funcUsesTexture = true }
            if line.contains("u_resolution") { funcUsesResolution = true }
            if line.contains("u_time") { funcUsesTime = true }
            braceDepth += countBraces(line)
            if braceDepth <= 0 {
                // Function ended — add extra params as needed
                if funcUsesTexture || funcUsesResolution || funcUsesTime {
                    var extraParams: [String] = []
                    if funcUsesTexture {
                        extraParams.append("texture2d<float> u_screen")
                        extraParams.append("sampler samp")
                        funcsNeedingTexture.insert(funcName)
                    }
                    if funcUsesResolution {
                        extraParams.append("float2 u_resolution")
                        funcsNeedingResolution.insert(funcName)
                    }
                    if funcUsesTime {
                        extraParams.append("float u_time")
                        funcsNeedingTime.insert(funcName)
                    }
                    funcLines = addExtraParams(funcLines, extraParams)
                }
                output.append(contentsOf: funcLines)
                inFunction = false
                funcLines = []
            }
        }
    }

    if !funcLines.isEmpty {
        output.append(contentsOf: funcLines)
    }

    var result = output.joined(separator: "\n")

    // Build fixups list for fixing call sites (both in helpers and in mainBody)
    var fixups: [(name: String, extraArgs: [String])] = []
    for name in funcsNeedingTexture.union(funcsNeedingResolution).union(funcsNeedingTime) {
        var extraArgs: [String] = []
        if funcsNeedingTexture.contains(name) {
            extraArgs.append("u_screen")
            extraArgs.append("samp")
        }
        if funcsNeedingResolution.contains(name) { extraArgs.append("u_resolution") }
        if funcsNeedingTime.contains(name) { extraArgs.append("u_time") }
        result = addExtraArgsToCallSites(result, funcName: name, extraArgs: extraArgs)
        fixups.append((name: name, extraArgs: extraArgs))
    }

    return (code: result, fixups: fixups)
}

/// Extract function name from a function definition line
func extractFuncName(_ line: String) -> String {
    // Pattern: "type name(..."
    let types = ["float", "float2", "float3", "float4", "int", "void",
                 "float2x2", "float3x3", "float4x4", "half", "half2", "half3", "half4"]
    for t in types {
        if line.hasPrefix(t + " ") {
            let rest = line.dropFirst(t.count + 1).trimmingCharacters(in: .whitespaces)
            if let parenIdx = rest.firstIndex(of: "(") {
                return String(rest[rest.startIndex..<parenIdx]).trimmingCharacters(in: .whitespaces)
            }
        }
    }
    return ""
}

/// Add extra params to a function's signature
func addExtraParams(_ funcLines: [String], _ extraParams: [String]) -> [String] {
    guard var firstLine = funcLines.first else { return funcLines }
    if let parenIdx = firstLine.lastIndex(of: ")") {
        let before = String(firstLine[firstLine.startIndex..<parenIdx])
        let after = String(firstLine[parenIdx...])
        if before.contains("(") {
            let openParen = before.lastIndex(of: "(")!
            let params = String(before[before.index(after: openParen)...]).trimmingCharacters(in: .whitespaces)
            let extra = extraParams.joined(separator: ", ")
            if params.isEmpty {
                firstLine = before + extra + after
            } else {
                firstLine = before + ", " + extra + after
            }
        }
    }
    var result = [firstLine]
    result.append(contentsOf: funcLines.dropFirst())
    return result
}

/// Add extra arguments to all call sites of a function
func addExtraArgsToCallSites(_ code: String, funcName: String, extraArgs: [String]) -> String {
    let pattern = funcName + "("
    var result = ""
    var i = code.startIndex

    while i < code.endIndex {
        if code[i...].hasPrefix(pattern) {
            // Skip function definitions — detected by a return type keyword before the name
            var isDefinition = false
            if i > code.startIndex {
                // Walk backwards past whitespace to find the preceding word
                var back = code.index(before: i)
                while back > code.startIndex && code[back] == " " {
                    back = code.index(before: back)
                }
                // Check if the preceding word is a type keyword
                let defTypes = ["float", "float2", "float3", "float4", "int", "void",
                                "float2x2", "float3x3", "float4x4", "half", "half2", "half3", "half4"]
                let preceding = String(code[code.startIndex...back])
                for t in defTypes {
                    if preceding.hasSuffix(t) { isDefinition = true; break }
                }
            }
            if !isDefinition {
                let argsStart = code.index(i, offsetBy: pattern.count)
                if let closeIdx = findMatchingParen(code, from: argsStart) {
                    let existingArgs = String(code[argsStart..<closeIdx]).trimmingCharacters(in: .whitespaces)
                    let extra = extraArgs.joined(separator: ", ")
                    if existingArgs.isEmpty {
                        result += funcName + "(" + extra + ")"
                    } else {
                        result += funcName + "(" + existingArgs + ", " + extra + ")"
                    }
                    i = code.index(after: closeIdx)
                    continue
                }
            }
        }
        result.append(code[i])
        i = code.index(after: i)
    }
    return result
}

/// Find the index of the matching closing parenthesis
func findMatchingParen(_ code: String, from start: String.Index) -> String.Index? {
    var depth = 0
    var idx = start
    while idx < code.endIndex {
        if code[idx] == "(" { depth += 1 }
        else if code[idx] == ")" {
            if depth == 0 { return idx }
            depth -= 1
        }
        idx = code.index(after: idx)
    }
    return nil
}

func isFunctionDefinition(_ line: String) -> Bool {
    // Match patterns like: float foo(...), vec3 bar(...), void baz(...)
    let types = ["float", "float2", "float3", "float4", "int", "void",
                 "float2x2", "float3x3", "float4x4", "half", "half2", "half3", "half4"]
    for t in types {
        if line.hasPrefix(t + " ") && line.contains("(") && !line.hasPrefix(t + " " + t) {
            // Make sure it's not a variable declaration
            let afterType = line.dropFirst(t.count + 1).trimmingCharacters(in: .whitespaces)
            if afterType.contains("(") {
                let nameEnd = afterType.firstIndex(of: "(") ?? afterType.endIndex
                let name = String(afterType[afterType.startIndex..<nameEnd])
                    .trimmingCharacters(in: .whitespaces)
                // Name should be a valid identifier
                if !name.isEmpty && !name.contains(" ") && !name.contains("=") {
                    return true
                }
            }
        }
    }
    return false
}

func countBraces(_ line: String) -> Int {
    var count = 0
    for ch in line {
        if ch == "{" { count += 1 }
        else if ch == "}" { count -= 1 }
    }
    return count
}


/// Fix early returns inside the fragment function: `return;` → `return _frag_out;`
func fixEarlyReturns(_ msl: String) -> String {
    // Only fix returns inside the fragment function body
    guard let fragStart = msl.range(of: "fragment float4 postprocess(") else { return msl }

    let before = String(msl[msl.startIndex..<fragStart.lowerBound])
    var after = String(msl[fragStart.lowerBound...])

    // Replace bare "return;" with "return _frag_out;" inside the fragment function
    after = after.replacingOccurrences(of: "        return;\n", with: "        return _frag_out;\n")
    after = after.replacingOccurrences(of: "    return;\n", with: "    return _frag_out;\n")
    // Catch any remaining bare returns
    let lines = after.components(separatedBy: "\n")
    var fixedLines: [String] = []
    for line in lines {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        if trimmed == "return;" {
            fixedLines.append(line.replacingOccurrences(of: "return;", with: "return _frag_out;"))
        } else {
            fixedLines.append(line)
        }
    }

    return before + fixedLines.joined(separator: "\n")
}


// MARK: - Metal Renderer

class ShaderRenderer: NSObject, MTKViewDelegate, SCStreamOutput {
    let device: MTLDevice
    let commandQueue: MTLCommandQueue
    var pipelineState: MTLRenderPipelineState?
    var samplerState: MTLSamplerState?
    var vertexBuffer: MTLBuffer?
    var uniformBuffer: MTLBuffer?

    var capturedTexture: MTLTexture?
    var textureCache: CVMetalTextureCache?
    let textureLock = NSLock()

    var startTime: CFAbsoluteTime = CFAbsoluteTimeGetCurrent()
    var resolution: SIMD2<Float> = .zero

    var shaderPath: String
    var customUniforms: [String: Float] = [:]

    // Uniform buffer layout (must match Metal struct)
    struct Uniforms {
        var resolution: SIMD2<Float>
        var time: Float
        var padding: Float = 0  // alignment
        // Custom uniforms follow
    }

    init(device: MTLDevice, shaderPath: String) {
        self.device = device
        self.commandQueue = device.makeCommandQueue()!
        self.shaderPath = shaderPath
        super.init()

        // Create texture cache for IOSurface → MTLTexture
        CVMetalTextureCacheCreate(nil, nil, device, nil, &textureCache)

        setupSampler()
        setupVertexBuffer()
        loadShader()
    }

    func setupSampler() {
        let desc = MTLSamplerDescriptor()
        desc.minFilter = .linear
        desc.magFilter = .linear
        desc.sAddressMode = .clampToEdge
        desc.tAddressMode = .clampToEdge
        samplerState = device.makeSamplerState(descriptor: desc)
    }

    func setupVertexBuffer() {
        // Full-screen quad: position (x,y) + texcoord (u,v)
        let vertices: [Float] = [
            // position    texcoord
            -1, -1,        0, 1,    // bottom-left  (flip Y: texcoord v=1)
             1, -1,        1, 1,    // bottom-right
            -1,  1,        0, 0,    // top-left     (flip Y: texcoord v=0)
             1,  1,        1, 0,    // top-right
        ]
        vertexBuffer = device.makeBuffer(bytes: vertices,
                                         length: vertices.count * MemoryLayout<Float>.stride,
                                         options: .storageModeShared)
    }

    func loadShader() {
        do {
            let glslSource = try String(contentsOfFile: shaderPath, encoding: .utf8)
            let mslSource = convertGLSLtoMSL(glslSource)

            // Vertex shader is always the same simple passthrough
            let vertexMSL = """
            #include <metal_stdlib>
            using namespace metal;

            struct VertexIn {
                float2 position [[attribute(0)]];
                float2 texcoord [[attribute(1)]];
            };

            struct VertexOut {
                float4 position [[position]];
                float2 texcoord;
            };

            vertex VertexOut vertex_main(
                uint vid [[vertex_id]],
                const device float4* vertices [[buffer(0)]]
            ) {
                VertexOut out;
                float4 v = vertices[vid];
                out.position = float4(v.xy, 0.0, 1.0);
                out.texcoord = v.zw;
                return out;
            }
            """

            let vertexLib = try device.makeLibrary(source: vertexMSL, options: nil)
            let fragmentLib = try device.makeLibrary(source: mslSource, options: nil)

            guard let vertexFunc = vertexLib.makeFunction(name: "vertex_main"),
                  let fragmentFunc = fragmentLib.makeFunction(name: "postprocess") else {
                fputs("Error: Could not find shader functions\n", stderr)
                exit(1)
            }

            let pipelineDesc = MTLRenderPipelineDescriptor()
            pipelineDesc.vertexFunction = vertexFunc
            pipelineDesc.fragmentFunction = fragmentFunc
            pipelineDesc.colorAttachments[0].pixelFormat = .bgra8Unorm
            // Enable blending for transparent overlay
            pipelineDesc.colorAttachments[0].isBlendingEnabled = false

            pipelineState = try device.makeRenderPipelineState(descriptor: pipelineDesc)

            // Allocate uniform buffer (enough for base uniforms + custom)
            let uniformSize = MemoryLayout<Uniforms>.stride + customUniforms.count * MemoryLayout<Float>.stride
            uniformBuffer = device.makeBuffer(length: max(uniformSize, 256),
                                              options: .storageModeShared)

        } catch {
            fputs("Error loading shader: \(error)\n", stderr)
            exit(1)
        }
    }

    // SCStreamOutput — receives captured frames
    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer,
                of type: SCStreamOutputType) {
        guard type == .screen else { return }
        guard let pixelBuffer = sampleBuffer.imageBuffer else { return }

        let width = CVPixelBufferGetWidth(pixelBuffer)
        let height = CVPixelBufferGetHeight(pixelBuffer)

        guard let cache = textureCache else { return }

        var cvTexture: CVMetalTexture?
        let status = CVMetalTextureCacheCreateTextureFromImage(
            nil, cache, pixelBuffer, nil,
            .bgra8Unorm, width, height, 0, &cvTexture
        )

        guard status == kCVReturnSuccess, let cvTex = cvTexture,
              let texture = CVMetalTextureGetTexture(cvTex) else { return }

        textureLock.lock()
        capturedTexture = texture
        resolution = SIMD2<Float>(Float(width), Float(height))
        textureLock.unlock()
    }

    // MTKViewDelegate — render each frame
    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        textureLock.lock()
        let texture = capturedTexture
        textureLock.unlock()

        guard let texture = texture,
              let pipelineState = pipelineState,
              let drawable = view.currentDrawable,
              let descriptor = view.currentRenderPassDescriptor else { return }

        // Update uniforms
        let time = Float(CFAbsoluteTimeGetCurrent() - startTime)
        var uniforms = Uniforms(resolution: resolution, time: time)
        uniformBuffer?.contents().copyMemory(from: &uniforms, byteCount: MemoryLayout<Uniforms>.stride)

        guard let commandBuffer = commandQueue.makeCommandBuffer(),
              let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: descriptor) else { return }

        encoder.setRenderPipelineState(pipelineState)
        encoder.setVertexBuffer(vertexBuffer, offset: 0, index: 0)
        encoder.setFragmentTexture(texture, index: 0)
        encoder.setFragmentSamplerState(samplerState, index: 0)
        encoder.setFragmentBuffer(uniformBuffer, offset: 0, index: 0)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        encoder.endEncoding()

        commandBuffer.present(drawable)
        commandBuffer.commit()
    }
}


// MARK: - Overlay Window

class OverlayWindow: NSWindow {
    init(screen: NSScreen) {
        super.init(
            contentRect: screen.frame,
            styleMask: .borderless,
            backing: .buffered,
            defer: false
        )

        self.level = .screenSaver
        self.isOpaque = false
        self.backgroundColor = .clear
        self.ignoresMouseEvents = true
        self.hasShadow = false
        self.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary]

        // Keep window on top and spanning the full screen
        self.setFrame(screen.frame, display: true)
    }
}


// MARK: - Application Delegate

class AppDelegate: NSObject, NSApplicationDelegate {
    var overlayWindow: OverlayWindow?
    var mtkView: MTKView?
    var renderer: ShaderRenderer?
    var stream: SCStream?
    var shaderPath: String

    // File watcher for hot-reload
    var fileWatcherSource: DispatchSourceFileSystemObject?

    // Signal handler sources (must be retained)
    var sigintSource: DispatchSourceSignal?
    var sigtermSource: DispatchSourceSignal?
    var sigusr1Source: DispatchSourceSignal?

    init(shaderPath: String) {
        self.shaderPath = shaderPath
        super.init()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        guard let device = MTLCreateSystemDefaultDevice() else {
            fputs("Error: Metal is not supported on this system\n", stderr)
            NSApp.terminate(nil)
            return
        }

        guard let mainScreen = NSScreen.main else {
            fputs("Error: No main screen found\n", stderr)
            NSApp.terminate(nil)
            return
        }

        // Create overlay window
        overlayWindow = OverlayWindow(screen: mainScreen)

        // Create Metal view
        let metalView = MTKView(frame: mainScreen.frame, device: device)
        metalView.isPaused = false
        metalView.enableSetNeedsDisplay = false
        metalView.preferredFramesPerSecond = 60
        metalView.colorPixelFormat = .bgra8Unorm
        metalView.layer?.isOpaque = false
        metalView.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        mtkView = metalView

        // Create renderer
        renderer = ShaderRenderer(device: device, shaderPath: shaderPath)
        metalView.delegate = renderer

        overlayWindow?.contentView = metalView
        overlayWindow?.makeKeyAndOrderFront(nil)

        // Start screen capture
        startCapture()

        // Watch shader file for hot-reload
        watchShaderFile()

        // Handle SIGUSR1 for reload
        setupSignalHandlers()

        fputs("screenshader-macos: overlay active with \(shaderPath)\n", stderr)
    }

    func startCapture() {
        // Get shareable content and start capturing
        SCShareableContent.getExcludingDesktopWindows(false, onScreenWindowsOnly: true) {
            [weak self] content, error in
            guard let self = self else { return }

            if let error = error {
                fputs("Error getting shareable content: \(error)\n", stderr)
                fputs("Make sure Screen Recording permission is granted.\n", stderr)
                DispatchQueue.main.async { NSApp.terminate(nil) }
                return
            }

            guard let content = content,
                  let display = content.displays.first else {
                fputs("Error: No display found\n", stderr)
                DispatchQueue.main.async { NSApp.terminate(nil) }
                return
            }

            // Exclude our own overlay window from capture to prevent feedback loop
            let excludedApps = content.applications.filter {
                $0.bundleIdentifier == Bundle.main.bundleIdentifier
            }

            let filter = SCContentFilter(display: display,
                                         excludingApplications: excludedApps,
                                         exceptingWindows: [])

            let config = SCStreamConfiguration()
            config.width = Int(display.width) * 2   // Retina
            config.height = Int(display.height) * 2
            config.minimumFrameInterval = CMTime(value: 1, timescale: 60)
            config.pixelFormat = kCVPixelFormatType_32BGRA
            config.queueDepth = 3
            config.showsCursor = true

            do {
                let stream = SCStream(filter: filter, configuration: config, delegate: nil)
                try stream.addStreamOutput(self.renderer!, type: .screen,
                                          sampleHandlerQueue: DispatchQueue.global(qos: .userInteractive))
                stream.startCapture { error in
                    if let error = error {
                        fputs("Error starting capture: \(error)\n", stderr)
                        DispatchQueue.main.async { NSApp.terminate(nil) }
                    }
                }
                self.stream = stream
            } catch {
                fputs("Error setting up stream: \(error)\n", stderr)
                DispatchQueue.main.async { NSApp.terminate(nil) }
            }
        }
    }

    func watchShaderFile() {
        let fd = open(shaderPath, O_EVTONLY)
        guard fd >= 0 else { return }

        let source = DispatchSource.makeFileSystemObjectSource(
            fileDescriptor: fd,
            eventMask: [.write, .rename],
            queue: DispatchQueue.main
        )

        source.setEventHandler { [weak self] in
            guard let self = self else { return }
            fputs("Shader file changed, reloading...\n", stderr)
            // Small delay to let write complete
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                self.renderer?.loadShader()
            }
        }

        source.setCancelHandler {
            close(fd)
        }

        source.resume()
        fileWatcherSource = source
    }

    func setupSignalHandlers() {
        // SIGUSR1 = reload shader
        signal(SIGUSR1, SIG_IGN)
        sigusr1Source = DispatchSource.makeSignalSource(signal: SIGUSR1, queue: .main)
        sigusr1Source?.setEventHandler { [weak self] in
            fputs("Received SIGUSR1, reloading shader...\n", stderr)
            self?.renderer?.loadShader()
        }
        sigusr1Source?.resume()

        // SIGTERM/SIGINT = clean exit
        signal(SIGTERM, SIG_IGN)
        sigtermSource = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
        sigtermSource?.setEventHandler {
            NSApp.terminate(nil)
        }
        sigtermSource?.resume()

        signal(SIGINT, SIG_IGN)
        sigintSource = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
        sigintSource?.setEventHandler {
            NSApp.terminate(nil)
        }
        sigintSource?.resume()
    }

    func applicationWillTerminate(_ notification: Notification) {
        stream?.stopCapture { _ in }
        fileWatcherSource?.cancel()
        fputs("screenshader-macos: stopped\n", stderr)
    }
}


// MARK: - Main

func listShaders() {
    let scriptDir = URL(fileURLWithPath: CommandLine.arguments[0])
        .deletingLastPathComponent()  // macos/
        .deletingLastPathComponent()  // project root
    let shadersDir = scriptDir.appendingPathComponent("shaders")

    guard let enumerator = FileManager.default.enumerator(
        at: shadersDir,
        includingPropertiesForKeys: nil,
        options: [.skipsSubdirectoryDescendants]
    ) else {
        print("No shaders directory found")
        return
    }

    print("Available shaders:")
    while let url = enumerator.nextObject() as? URL {
        if url.pathExtension == "frag" {
            let name = url.deletingPathExtension().lastPathComponent
            if name != "composite" {
                print("  \(name)  (\(url.path))")
            }
        }
    }
}

// Parse CLI arguments
let args = CommandLine.arguments

if args.count > 1 {
    switch args[1] {
    case "--stop":
        // Stop is handled by the shell script (kill by PID)
        print("Use screenshader.sh --stop to stop the overlay")
        exit(0)
    case "--list":
        listShaders()
        exit(0)
    case "--help", "-h":
        print("Usage: screenshader-macos [shader.frag]")
        print("       screenshader-macos --list")
        print("       screenshader-macos --help")
        exit(0)
    default:
        break
    }
}

let shaderPath: String
if args.count > 1 && !args[1].hasPrefix("-") {
    shaderPath = args[1]
} else {
    // Default shader
    let scriptDir = URL(fileURLWithPath: args[0])
        .deletingLastPathComponent()
        .deletingLastPathComponent()
    shaderPath = scriptDir.appendingPathComponent("shaders/crt.frag").path
}

// Verify shader exists
guard FileManager.default.fileExists(atPath: shaderPath) else {
    fputs("Error: shader not found: \(shaderPath)\n", stderr)
    exit(1)
}

// Launch the app
let app = NSApplication.shared
app.setActivationPolicy(.accessory) // No dock icon
let delegate = AppDelegate(shaderPath: shaderPath)
app.delegate = delegate
app.run()
