// The internal resolution as gs_renderer.h's GsScale has it: an image's
// pixels to a GS pixel, each way, in 1/1024ths (1024: the PS2's own). Image
// pixel i samples the GS at s(i) = (i + 0.5) / k - 0.5 (gs.vert's half-pixel
// move); GS pixel x owns the image pixels whose s(i) lies in [x, x + 1),
// ScaleEdge(x) to ScaleEdge(x + 1) (clamped to the image), and ScaleMid(x),
// the first of them, is the one taken to memory - x * k + k / 2 at a whole
// factor k. The same integer sums as the C++ (gs_renderer.h), so both sides
// agree to the pixel.

uint ScaleSize(uint size, uint q) { return (size * q + 1023u) >> 10; }
uint ScaleMid(uint at, uint q) { return ((2u * at + 1u) * q + 1023u) >> 11; }
uint ScaleEdge(uint at, uint q) { return at == 0u ? 0u : ScaleMid(at, q); }
// The GS pixel owning an image pixel (ScaleEdge inverted): 0 for those sampling left of it.
uint ScaleBack(uint pixel, uint q) {
    const uint twice = (2u * pixel + 1u) * 1024u;
    return twice <= q ? 0u : (twice - q) / (2u * q);
}

uvec2 ScaleSize(uvec2 size, uvec2 q) { return uvec2(ScaleSize(size.x, q.x), ScaleSize(size.y, q.y)); }
uvec2 ScaleEdge(uvec2 at, uvec2 q) { return uvec2(ScaleEdge(at.x, q.x), ScaleEdge(at.y, q.y)); }
uvec2 ScaleMid(uvec2 at, uvec2 q) { return uvec2(ScaleMid(at.x, q.x), ScaleMid(at.y, q.y)); }
uvec2 ScaleBack(uvec2 pixel, uvec2 q) { return uvec2(ScaleBack(pixel.x, q.x), ScaleBack(pixel.y, q.y)); }
