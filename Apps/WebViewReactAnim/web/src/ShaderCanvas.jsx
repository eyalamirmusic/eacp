import { useCallback, useRef } from 'react';
import { createShaderRenderer } from './shader';

const FRAGMENT = `
precision highp float;
uniform vec2 uResolution;
uniform float uAngle;

const float POWER = 8.0;
const int BULB_ITERS = 7;
const int MARCH_STEPS = 80;
const float MAX_DIST = 6.0;
const float HIT = 0.0008;

float mandelbulb(vec3 p)
{
    vec3 z = p;
    float dr = 1.0;
    float r = 0.0;

    for (int i = 0; i < BULB_ITERS; i++)
    {
        r = length(z);
        if (r > 2.0) break;

        float theta = acos(z.z / r) * POWER;
        float phi = atan(z.y, z.x) * POWER;
        float zr = pow(r, POWER);
        dr = pow(r, POWER - 1.0) * POWER * dr + 1.0;

        z = zr * vec3(sin(theta) * cos(phi),
                      sin(theta) * sin(phi),
                      cos(theta));
        z += p;
    }
    return 0.5 * log(r) * r / dr;
}

vec3 normalAt(vec3 p)
{
    vec2 h = vec2(0.0008, 0.0);
    return normalize(vec3(
        mandelbulb(p + h.xyy) - mandelbulb(p - h.xyy),
        mandelbulb(p + h.yxy) - mandelbulb(p - h.yxy),
        mandelbulb(p + h.yyx) - mandelbulb(p - h.yyx)));
}

mat3 rotY(float a)
{
    float c = cos(a), s = sin(a);
    return mat3(c, 0.0, -s, 0.0, 1.0, 0.0, s, 0.0, c);
}

mat3 rotX(float a)
{
    float c = cos(a), s = sin(a);
    return mat3(1.0, 0.0, 0.0, 0.0, c, -s, 0.0, s, c);
}

void main()
{
    vec2 uv = (gl_FragCoord.xy - 0.5 * uResolution)
              / min(uResolution.x, uResolution.y);
    float t = radians(uAngle);

    mat3 view = rotY(t) * rotX(sin(t) * 0.5);
    vec3 ro = view * vec3(0.0, 0.0, 2.7);
    vec3 forward = normalize(-ro);
    vec3 right = normalize(cross(vec3(0.0, 1.0, 0.0), forward));
    vec3 up = cross(forward, right);
    vec3 rd = normalize(forward + uv.x * right + uv.y * up);

    float total = 0.0;
    bool hit = false;
    vec3 p = ro;

    for (int i = 0; i < MARCH_STEPS; i++)
    {
        p = ro + rd * total;
        float d = mandelbulb(p);
        if (d < HIT) { hit = true; break; }
        if (total > MAX_DIST) break;
        total += d * 0.75;
    }

    vec3 col;
    if (hit)
    {
        vec3 n = normalAt(p);
        vec3 lightDir = normalize(view * vec3(0.6, 0.7, -0.5));
        float diff = max(dot(n, lightDir), 0.0);
        float fres = pow(1.0 - max(dot(-rd, n), 0.0), 3.0);
        float ao = 1.0 / (1.0 + float(MARCH_STEPS) * 0.02);

        vec3 base = 0.5 + 0.5 * cos(vec3(0.0, 2.094, 4.188)
                                  + length(p) * 4.0 + t);
        col = base * (0.15 + diff * ao) + fres * vec3(0.5, 0.7, 1.0);
    }
    else
    {
        col = mix(vec3(0.02, 0.03, 0.08),
                  vec3(0.06, 0.04, 0.12),
                  0.5 + 0.5 * uv.y);
    }

    col = pow(col, vec3(0.85));
    gl_FragColor = vec4(col, 1.0);
}
`;

export default function ShaderCanvas({ angle, width = 560, height = 320 })
{
    const rendererRef = useRef(null);

    const setCanvas = useCallback((canvas) =>
    {
        if (!canvas)
        {
            rendererRef.current?.dispose();
            rendererRef.current = null;
            return;
        }
        const dpr = window.devicePixelRatio || 1;
        canvas.width = width * dpr;
        canvas.height = height * dpr;
        rendererRef.current = createShaderRenderer(canvas, FRAGMENT);
    }, [width, height]);

    rendererRef.current?.draw(angle);

    return (
        <canvas ref={setCanvas} className="shader" style={{ width, height }} />
    );
}
