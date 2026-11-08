// Dark Realm - Stufe 1 (R36S / ArkOS), SDL2 + OpenGL ES 2.0
// Hauptmenue mit einem Spielstand, Erfahrungspunkte & Level, Inventar,
// Gegner lassen Items fallen, Lagerfeuer = speichern & heilen.
//
// Handheld: Linker Stick = laufen, Rechter Stick = Kamera, A = Angriff,
//           X = Heiltrank, Y = Inventar, Start = Hauptmenue (speichert),
//           Start+Select = Beenden. Menue: D-Pad hoch/runter, A = OK.
// PC:       WASD laufen, Pfeil links/rechts Kamera, Leertaste Angriff,
//           Q Heiltrank, I Inventar, M Hauptmenue, Enter OK, ESC Beenden.
#include <SDL.h>
#include <SDL_opengles2.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// ---------- Mathe ----------
struct Mat { float m[16]; };

static Mat identity() { Mat r; memset(r.m, 0, sizeof r.m); r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }

static Mat mul(const Mat& a, const Mat& b) {
    Mat r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

static Mat perspective(float fovy, float asp, float n, float f) {
    Mat r; memset(r.m, 0, sizeof r.m);
    float t = 1.f / tanf(fovy * 0.5f);
    r.m[0] = t / asp; r.m[5] = t;
    r.m[10] = (f + n) / (n - f); r.m[11] = -1.f;
    r.m[14] = 2.f * f * n / (n - f);
    return r;
}

static Mat lookAt(float ex, float ey, float ez, float cx, float cy, float cz) {
    float fx = cx - ex, fy = cy - ey, fz = cz - ez;
    float fl = sqrtf(fx * fx + fy * fy + fz * fz); fx /= fl; fy /= fl; fz /= fl;
    float sx = -fz, sy = 0, sz = fx;
    float sl = sqrtf(sx * sx + sz * sz); sx /= sl; sz /= sl;
    float ux = sy * fz - sz * fy, uy = sz * fx - sx * fz, uz = sx * fy - sy * fx;
    Mat r = identity();
    r.m[0] = sx; r.m[4] = sy; r.m[8] = sz;
    r.m[1] = ux; r.m[5] = uy; r.m[9] = uz;
    r.m[2] = -fx; r.m[6] = -fy; r.m[10] = -fz;
    r.m[12] = -(sx * ex + sy * ey + sz * ez);
    r.m[13] = -(ux * ex + uy * ey + uz * ez);
    r.m[14] = fx * ex + fy * ey + fz * ez;
    return r;
}

// Modell: Translation * RotationY * Skalierung
static Mat model(float x, float y, float z, float a, float sx, float sy, float sz) {
    Mat r = identity();
    float c = cosf(a), s = sinf(a);
    r.m[0] = c * sx;  r.m[2] = -s * sx;
    r.m[5] = sy;
    r.m[8] = s * sz;  r.m[10] = c * sz;
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}

static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

// ---------- 3D-Shader ----------
static const char* VS =
    "attribute vec3 aPos; attribute vec3 aNor; attribute vec3 aCol;\n"
    "uniform mat4 uVP; uniform mat4 uModel; uniform float uRot;\n"
    "uniform vec3 uTint; uniform vec3 uCam; uniform vec4 uFire; uniform float uFogMul;\n"
    "varying vec3 vCol; varying float vFog;\n"
    "void main(){\n"
    "  vec4 wp = uModel * vec4(aPos,1.0);\n"
    "  gl_Position = uVP * wp;\n"
    "  float c = cos(uRot), s = sin(uRot);\n"
    "  vec3 n = vec3(aNor.x*c + aNor.z*s, aNor.y, -aNor.x*s + aNor.z*c);\n"
    "  float d = max(dot(normalize(n), normalize(vec3(0.4,0.8,0.3))), 0.0);\n"
    "  float fl = clamp(1.0 - length(wp.xyz - uFire.xyz) / 16.0, 0.0, 1.0);\n"
    "  vec3 light = vec3(0.40,0.45,0.65) + vec3(0.50,0.55,0.80)*d + vec3(1.0,0.55,0.2)*fl*fl*uFire.w*1.8;\n"
    "  vCol = aCol * uTint * light;\n"
    "  gl_PointSize = 2.0;\n"
    "  vFog = clamp((length(wp.xyz - uCam) - 15.0) / 45.0, 0.0, 1.0) * uFogMul;\n"
    "}\n";

static const char* FS =
    "precision mediump float;\n"
    "varying vec3 vCol; varying float vFog;\n"
    "uniform vec3 uFogCol;\n"
    "void main(){ gl_FragColor = vec4(mix(vCol, uFogCol, vFog), 1.0); }\n";

// ---------- 2D-Shader (Menue, Text, Leisten) ----------
static const char* VS2 =
    "attribute vec2 aPos; attribute vec2 aUV; uniform vec2 uScreen; varying vec2 vUV;\n"
    "void main(){ vUV = aUV; gl_Position = vec4(aPos.x/uScreen.x*2.0-1.0, 1.0-aPos.y/uScreen.y*2.0, 0.0, 1.0); }\n";

static const char* FS2 =
    "precision mediump float; varying vec2 vUV;\n"
    "uniform sampler2D uTex; uniform vec4 uCol; uniform float uUseTex;\n"
    "void main(){ float a = mix(1.0, texture2D(uTex, vUV).a, uUseTex); gl_FragColor = vec4(uCol.rgb, uCol.a * a); }\n";

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, NULL, log); fprintf(stderr, "Shader-Fehler: %s\n", log); }
    return s;
}

static GLuint prog, prog2d, fontTex;
static GLint uVP, uModel, uRot, uTint, uCam, uFogCol, uFire, uFogMul;
static GLint u2Screen, u2Col, u2UseTex, u2Tex;

// ---------- 5x7-Schriftart (Leerzeichen bis Z, nur Grossbuchstaben) ----------
static const unsigned char FONT[59][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43}
};

static void buildFont() {
    unsigned char atlas[128 * 32];
    memset(atlas, 0, sizeof atlas);
    for (int i = 0; i < 59; i++) {
        int cx = (i % 16) * 8, cy = (i / 16) * 8;
        for (int col = 0; col < 5; col++)
            for (int r = 0; r < 7; r++)
                if (FONT[i][col] & (1 << r)) atlas[(cy + r) * 128 + cx + col] = 255;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(1, &fontTex);
    glBindTexture(GL_TEXTURE_2D, fontTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, 128, 32, 0, GL_ALPHA, GL_UNSIGNED_BYTE, atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// ---------- 2D-Zeichnen (virtueller Bildschirm 320x240) ----------
static void begin2D() {
    glUseProgram(prog2d);
    glUniform2f(u2Screen, 320.0f, 240.0f);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisableVertexAttribArray(2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
}

static void quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1) {
    float v[24] = { x, y, u0, v0,  x + w, y, u1, v0,  x, y + h, u0, v1,
                    x + w, y, u1, v0,  x + w, y + h, u1, v1,  x, y + h, u0, v1 };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    glUniform1f(u2UseTex, 0.0f);
    glUniform4f(u2Col, r, g, b, a);
    quad(x, y, w, h, 0, 0, 0, 0);
}

static float textWidth(const char* s, float sc) { return (float)strlen(s) * 6.0f * sc; }

static void text(const char* s, float x, float y, float sc, float r, float g, float b, float a = 1.0f) {
    glUniform1f(u2UseTex, 1.0f);
    glUniform4f(u2Col, r, g, b, a);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fontTex);
    glUniform1i(u2Tex, 0);
    for (; *s; s++) {
        int c = toupper((unsigned char)*s);
        if (c < 0x20 || c > 0x5A) c = 0x20;
        int i = c - 0x20;
        float cx = (float)((i % 16) * 8), cy = (float)((i / 16) * 8);
        quad(x, y, 6 * sc, 8 * sc, cx / 128.0f, cy / 32.0f, (cx + 6) / 128.0f, (cy + 8) / 32.0f);
        x += 6 * sc;
    }
}

static void end2D() {
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glUseProgram(prog);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
}

// ---------- 3D-Meshes ----------
struct Mesh { GLuint vbo = 0, ibo = 0; int count = 0; GLenum mode = GL_TRIANGLES; };

static Mesh makeMesh(const std::vector<float>& v, const std::vector<unsigned short>& idx) {
    Mesh m;
    glGenBuffers(1, &m.vbo); glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &m.ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(unsigned short), idx.data(), GL_STATIC_DRAW);
    m.count = (int)idx.size();
    return m;
}

static void drawMesh(const Mesh& m, const Mat& mdl, float rot, float r, float g, float b) {
    glUniformMatrix4fv(uModel, 1, GL_FALSE, mdl.m);
    glUniform1f(uRot, rot);
    glUniform3f(uTint, r, g, b);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 36, (void*)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 36, (void*)12);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 36, (void*)24);
    glDrawElements(m.mode, m.count, GL_UNSIGNED_SHORT, 0);
}

static Mesh makeCube() {
    std::vector<float> v; std::vector<unsigned short> idx;
    const float N[6][3][3] = {
        {{1,0,0},{0,1,0},{0,0,1}}, {{-1,0,0},{0,0,1},{0,1,0}},
        {{0,1,0},{0,0,1},{1,0,0}}, {{0,-1,0},{1,0,0},{0,0,1}},
        {{0,0,1},{1,0,0},{0,1,0}}, {{0,0,-1},{0,1,0},{1,0,0}} };
    for (int f = 0; f < 6; f++) {
        const float* n = N[f][0]; const float* u = N[f][1]; const float* w = N[f][2];
        int base = (int)(v.size() / 9);
        for (int c = 0; c < 4; c++) {
            float su = (c & 1) ? 0.5f : -0.5f, sw = (c & 2) ? 0.5f : -0.5f;
            for (int i = 0; i < 3; i++) v.push_back(n[i] * 0.5f + u[i] * su + w[i] * sw);
            for (int i = 0; i < 3; i++) v.push_back(n[i]);
            v.push_back(1); v.push_back(1); v.push_back(1);
        }
        unsigned short q[6] = {0, 1, 2, 1, 3, 2};
        for (int i = 0; i < 6; i++) idx.push_back((unsigned short)(base + q[i]));
    }
    return makeMesh(v, idx);
}

// ---------- Welt ----------
static float height(float x, float z) {
    return 3.0f * sinf(x * 0.08f) * cosf(z * 0.07f) + 1.5f * sinf(x * 0.19f + z * 0.13f) + 0.6f * sinf(z * 0.31f);
}

static float frand() { return rand() / (float)RAND_MAX; }

static Mesh makeTerrain() {
    const int N = 128; const float cell = 2.0f;
    std::vector<float> v; std::vector<unsigned short> idx;
    for (int j = 0; j <= N; j++)
        for (int i = 0; i <= N; i++) {
            float x = (i - N / 2) * cell, z = (j - N / 2) * cell, h = height(x, z);
            float nx = height(x - 1, z) - height(x + 1, z), ny = 2.0f, nz = height(x, z - 1) - height(x, z + 1);
            float nl = sqrtf(nx * nx + ny * ny + nz * nz);
            float g = fminf(fmaxf(h / 4.0f * 0.5f + 0.5f, 0.0f), 1.0f);
            float noise = ((i * 73 + j * 151) % 7) * 0.006f;
            v.push_back(x); v.push_back(h); v.push_back(z);
            v.push_back(nx / nl); v.push_back(ny / nl); v.push_back(nz / nl);
            v.push_back(0.10f + 0.10f * g + noise); v.push_back(0.14f + 0.08f * g + noise); v.push_back(0.09f + 0.04f * g);
        }
    for (int j = 0; j < N; j++)
        for (int i = 0; i < N; i++) {
            unsigned short a = (unsigned short)(j * (N + 1) + i), b = a + 1, c = a + (N + 1), d = c + 1;
            unsigned short q[6] = {a, c, b, b, c, d};
            for (int k = 0; k < 6; k++) idx.push_back(q[k]);
        }
    return makeMesh(v, idx);
}

static Mesh makeStars() {
    std::vector<float> v; std::vector<unsigned short> idx;
    for (int i = 0; i < 400; i++) {
        float a = frand() * 6.2831f, e = 0.05f + frand() * 1.5f;
        float x = cosf(e) * cosf(a) * 90, y = sinf(e) * 90, z = cosf(e) * sinf(a) * 90;
        float br = 0.6f + frand() * 0.8f;
        v.push_back(x); v.push_back(y); v.push_back(z);
        v.push_back(0); v.push_back(1); v.push_back(0);
        v.push_back(br); v.push_back(br); v.push_back(br * 1.1f);
        idx.push_back((unsigned short)i);
    }
    Mesh m = makeMesh(v, idx); m.mode = GL_POINTS; return m;
}

// ---------- Spieldaten ----------
enum Item { IT_POTION, IT_BONE, IT_SHARD, IT_COUNT };
static const char* ITEM_NAME[IT_COUNT] = {"HEILTRANK", "KNOCHEN", "SEELENSPLITTER"};
static const char* ITEM_DESC[IT_COUNT] = {"HEILT 40 LEBEN", "EIN ALTER KNOCHEN", "SCHIMMERT SCHWACH VIOLETT"};

static int level = 1, xp = 0;
static int items[IT_COUNT] = {2, 0, 0};
static float hp = 100;
static float px = 0, pz = 0;

static int xpNeed() { return 20 + 20 * (level - 1); }
static int maxHp() { return 100 + 15 * (level - 1); }
static int dmgVal() { return 1 + (level - 1) / 3; }

static std::vector<std::pair<std::string, float> > msgs;
static void msg(const std::string& s) {
    msgs.push_back(std::make_pair(s, 3.5f));
    if (msgs.size() > 4) msgs.erase(msgs.begin());
}

static void addXp(int n) {
    xp += n;
    while (level < 20 && xp >= xpNeed()) {
        xp -= xpNeed(); level++; hp = (float)maxHp();
        char b[48]; snprintf(b, sizeof b, "LEVEL UP! LEVEL %d", level); msg(b);
    }
}

static const char* SAVE_FILE = "darkrealm.sav";

static bool saveExists() { FILE* f = fopen(SAVE_FILE, "r"); if (f) { fclose(f); return true; } return false; }

static bool saveGame() {
    FILE* f = fopen(SAVE_FILE, "w");
    if (!f) return false;
    fprintf(f, "%d %d %d %d %d %.2f %.2f\n", level, xp, items[0], items[1], items[2], px, pz);
    fclose(f);
    return true;
}

static bool loadGame() {
    FILE* f = fopen(SAVE_FILE, "r");
    if (!f) return false;
    int l, x, a, b, c; float X, Z;
    int n = fscanf(f, "%d %d %d %d %d %f %f", &l, &x, &a, &b, &c, &X, &Z);
    fclose(f);
    if (n != 7) return false;
    level = l < 1 ? 1 : (l > 20 ? 20 : l); xp = x < 0 ? 0 : x;
    items[0] = a; items[1] = b; items[2] = c;
    px = clampf(X, -120.f, 120.f); pz = clampf(Z, -120.f, 120.f);
    hp = (float)maxHp();
    return true;
}

struct Tree { float x, z; };
struct Enemy { float x, z, hp, flash; };
struct Pickup { float x, z; int item; };

static void respawn(Enemy& e, float cx, float cz) {
    float a = frand() * 6.2831f, d = 30.0f + frand() * 30.0f;
    e.x = clampf(cx + cosf(a) * d, -120.f, 120.f);
    e.z = clampf(cz + sinf(a) * d, -120.f, 120.f);
    e.hp = (float)(3 + (level - 1) / 3); e.flash = 0;
}

// ---------- Ritter ----------
static void drawKnight(const Mesh& cube, float kx, float kz, float face, float step, bool swing, float dmg) {
    float ky = height(kx, kz);
    float cf = cosf(face), sf = sinf(face);
    auto part = [&](float lx, float ly, float lz, float sx, float sy, float sz, float r, float g, float b) {
        float wx = kx + lx * cf + lz * sf, wz = kz - lx * sf + lz * cf;
        drawMesh(cube, model(wx, ky + ly, wz, face, sx, sy, sz), face,
                 r * (1.0f + dmg * 0.8f), g * (1.0f - dmg * 0.6f), b * (1.0f - dmg * 0.6f));
    };
    part(-0.22f, 0.45f, step, 0.3f, 0.9f, 0.32f, 0.33f, 0.35f, 0.40f);
    part( 0.22f, 0.45f, -step, 0.3f, 0.9f, 0.32f, 0.33f, 0.35f, 0.40f);
    part(0, 1.35f, 0, 0.9f, 0.9f, 0.55f, 0.50f, 0.53f, 0.58f);
    part(-0.65f, 1.72f, 0, 0.42f, 0.3f, 0.55f, 0.58f, 0.61f, 0.66f);
    part( 0.65f, 1.72f, 0, 0.42f, 0.3f, 0.55f, 0.58f, 0.61f, 0.66f);
    part(-0.65f, 1.15f, -step * 0.8f, 0.26f, 0.8f, 0.3f, 0.45f, 0.48f, 0.53f);
    part( 0.65f, 1.15f, step * 0.8f, 0.26f, 0.8f, 0.3f, 0.45f, 0.48f, 0.53f);
    part(0, 2.05f, 0, 0.52f, 0.55f, 0.52f, 0.55f, 0.58f, 0.63f);
    part(0, 2.05f, 0.27f, 0.34f, 0.07f, 0.06f, 0.02f, 0.02f, 0.03f);
    part(0, 1.4f, -0.36f, 0.95f, 1.5f, 0.1f, 0.5f, 0.32f, 0.12f);
    part(0.85f, 1.2f, 0.2f, 0.1f, 0.85f, 0.6f, 0.28f, 0.22f, 0.18f);
    if (swing) part(-0.65f, 1.3f, 1.3f, 0.12f, 0.12f, 1.9f, 0.85f, 0.85f, 0.95f);
    else part(-0.7f, 1.2f, 0.5f, 0.1f, 0.1f, 1.3f, 0.7f, 0.7f, 0.8f);
}

// ---------- Eingabe ----------
struct Pad { bool up = false, down = false, a = false, b = false, x = false, y = false, start = false, select = false; };

static float axisVal(SDL_GameController* p, SDL_GameControllerAxis a) {
    float v = SDL_GameControllerGetAxis(p, a) / 32768.0f;
    return fabsf(v) < 0.2f ? 0.0f : v;
}
static float joyVal(SDL_Joystick* j, int a) {
    float v = SDL_JoystickGetAxis(j, a) / 32768.0f;
    return fabsf(v) < 0.2f ? 0.0f : v;
}

enum State { ST_MENU, ST_PLAY, ST_INV };

int main(int argc, char** argv) {
    bool windowed = false;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--window")) windowed = true;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);

    Uint32 flags = SDL_WINDOW_OPENGL | (windowed ? SDL_WINDOW_SHOWN : SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_Window* win = SDL_CreateWindow("Dark Realm", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, flags);
    if (!win) { fprintf(stderr, "Fenster: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "GL-Kontext: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);
    SDL_ShowCursor(SDL_DISABLE);

    SDL_GameController* pad = NULL; SDL_Joystick* joy = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); break; }
    }
    if (!pad && SDL_NumJoysticks() > 0) joy = SDL_JoystickOpen(0);

    // 3D-Programm
    prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, FS));
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aNor");
    glBindAttribLocation(prog, 2, "aCol");
    glLinkProgram(prog);
    uVP = glGetUniformLocation(prog, "uVP"); uModel = glGetUniformLocation(prog, "uModel");
    uRot = glGetUniformLocation(prog, "uRot"); uTint = glGetUniformLocation(prog, "uTint");
    uCam = glGetUniformLocation(prog, "uCam"); uFogCol = glGetUniformLocation(prog, "uFogCol");
    uFire = glGetUniformLocation(prog, "uFire"); uFogMul = glGetUniformLocation(prog, "uFogMul");

    // 2D-Programm
    prog2d = glCreateProgram();
    glAttachShader(prog2d, compile(GL_VERTEX_SHADER, VS2));
    glAttachShader(prog2d, compile(GL_FRAGMENT_SHADER, FS2));
    glBindAttribLocation(prog2d, 0, "aPos");
    glBindAttribLocation(prog2d, 1, "aUV");
    glLinkProgram(prog2d);
    u2Screen = glGetUniformLocation(prog2d, "uScreen"); u2Col = glGetUniformLocation(prog2d, "uCol");
    u2UseTex = glGetUniformLocation(prog2d, "uUseTex"); u2Tex = glGetUniformLocation(prog2d, "uTex");
    buildFont();

    const float fog[3] = {0.04f, 0.06f, 0.12f};
    glUseProgram(prog);
    glUniform3f(uFogCol, fog[0], fog[1], fog[2]);
    glClearColor(fog[0], fog[1], fog[2], 1);

    Mesh terrain = makeTerrain();
    Mesh cube = makeCube();
    Mesh stars = makeStars();

    srand(7);
    std::vector<Tree> trees;
    while (trees.size() < 80) {
        float x = (frand() - 0.5f) * 230, z = (frand() - 0.5f) * 230;
        if (x * x + z * z > 36) { Tree t; t.x = x; t.z = z; trees.push_back(t); }
    }
    const float bx = 8.0f, bz = -6.0f;   // Lagerfeuer
    std::vector<Enemy> enemies(6);
    std::vector<Pickup> pickups;
    float face = 0, yaw = 0, atkCd = 0, swing = 0, tm = 0, walk = 0;
    bool nearFire = false;
    int menuSel = 0, invSel = 0;
    State state = ST_MENU;
    bool hasSave = saveExists();
    menuSel = hasSave ? 1 : 0;
    Pad prev;

    auto resetEnemies = [&]() { for (size_t i = 0; i < enemies.size(); i++) respawn(enemies[i], px, pz); };
    auto newGame = [&]() {
        level = 1; xp = 0; items[0] = 2; items[1] = 0; items[2] = 0;
        hp = (float)maxHp(); px = 0; pz = 0; yaw = 0;
        pickups.clear(); msgs.clear(); resetEnemies();
        msg("A: ANGREIFEN   X: HEILTRANK   Y: INVENTAR");
        msg("TOETE GEGNER FUER ERFAHRUNG UND BEUTE");
        msg("AM LAGERFEUER WIRST DU GEHEILT");
    };
    auto usePotion = [&]() {
        if (items[IT_POTION] > 0 && hp < (float)maxHp()) {
            items[IT_POTION]--; hp = fminf((float)maxHp(), hp + 40.0f); msg("HEILTRANK GETRUNKEN");
        } else if (items[IT_POTION] <= 0) msg("KEIN HEILTRANK");
    };

    Uint32 last = SDL_GetTicks();
    bool run = true;
    while (run) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) run = false;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) run = false;
        }
        Uint32 now = SDL_GetTicks();
        float dt = fminf((now - last) / 1000.0f, 0.05f); last = now; tm += dt;

        // --- Eingabe ---
        float lx = 0, ly = 0, rx = 0; bool atk = false;
        Pad cur;
        if (pad) {
            lx = axisVal(pad, SDL_CONTROLLER_AXIS_LEFTX); ly = axisVal(pad, SDL_CONTROLLER_AXIS_LEFTY);
            rx = axisVal(pad, SDL_CONTROLLER_AXIS_RIGHTX);
            atk = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) != 0;
            cur.up = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
            cur.down = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;
            cur.a = atk;
            cur.b = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) != 0;
            cur.x = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X) != 0;
            cur.y = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y) != 0;
            cur.start = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START) != 0;
            cur.select = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK) != 0;
        } else if (joy) {
            lx = joyVal(joy, 0); ly = joyVal(joy, 1); rx = joyVal(joy, 2);
            Uint8 hat = SDL_JoystickGetHat(joy, 0);
            cur.up = (hat & SDL_HAT_UP) != 0; cur.down = (hat & SDL_HAT_DOWN) != 0;
            atk = SDL_JoystickGetButton(joy, 0) != 0;
            cur.a = atk; cur.b = SDL_JoystickGetButton(joy, 1) != 0;
            cur.x = SDL_JoystickGetButton(joy, 2) != 0; cur.y = SDL_JoystickGetButton(joy, 3) != 0;
            cur.select = SDL_JoystickGetButton(joy, 6) != 0; cur.start = SDL_JoystickGetButton(joy, 7) != 0;
        }
        const Uint8* k = SDL_GetKeyboardState(NULL);
        if (k[SDL_SCANCODE_A]) lx -= 1; if (k[SDL_SCANCODE_D]) lx += 1;
        if (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_UP]) ly -= 1;
        if (k[SDL_SCANCODE_S] || k[SDL_SCANCODE_DOWN]) ly += 1;
        if (k[SDL_SCANCODE_LEFT]) rx -= 1; if (k[SDL_SCANCODE_RIGHT]) rx += 1;
        if (k[SDL_SCANCODE_SPACE]) atk = true;
        if (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_SPACE]) cur.a = true;
        if (k[SDL_SCANCODE_BACKSPACE]) cur.b = true;
        if (k[SDL_SCANCODE_Q]) cur.x = true;
        if (k[SDL_SCANCODE_I]) cur.y = true;
        if (k[SDL_SCANCODE_M]) cur.start = true;
        if (ly < -0.6f) cur.up = true;
        if (ly > 0.6f) cur.down = true;
        Pad pr;
        pr.up = cur.up && !prev.up; pr.down = cur.down && !prev.down;
        pr.a = cur.a && !prev.a; pr.b = cur.b && !prev.b;
        pr.x = cur.x && !prev.x; pr.y = cur.y && !prev.y;
        pr.start = cur.start && !prev.start;
        prev = cur;

        if (cur.start && cur.select) run = false;

        for (size_t i = 0; i < msgs.size(); i++) msgs[i].second -= dt;
        while (!msgs.empty() && msgs[0].second <= 0) msgs.erase(msgs.begin());

        float py = height(px, pz);

        // --- Zustandslogik ---
        if (state == ST_MENU) {
            if (pr.up) menuSel = (menuSel + 2) % 3;
            if (pr.down) menuSel = (menuSel + 1) % 3;
            if (pr.a) {
                if (menuSel == 0) { newGame(); state = ST_PLAY; atkCd = 0.5f; nearFire = false; }
                else if (menuSel == 1 && hasSave) {
                    if (loadGame()) { pickups.clear(); msgs.clear(); resetEnemies(); state = ST_PLAY; atkCd = 0.5f; nearFire = false; }
                } else if (menuSel == 2) run = false;
            }
        } else if (state == ST_INV) {
            if (pr.up) invSel = (invSel + IT_COUNT - 1) % IT_COUNT;
            if (pr.down) invSel = (invSel + 1) % IT_COUNT;
            if (pr.a && invSel == IT_POTION) usePotion();
            if (pr.y || pr.b) { state = ST_PLAY; atkCd = 0.3f; }
        } else {
            if (pr.y) state = ST_INV;
            if (pr.x) usePotion();
            if (pr.start && !cur.select) { saveGame(); hasSave = true; state = ST_MENU; menuSel = 1; }

            // Spieler
            yaw -= rx * 2.2f * dt;
            float fwd = -ly, str = lx;
            float mx = -sinf(yaw) * fwd + cosf(yaw) * str;
            float mz = -cosf(yaw) * fwd - sinf(yaw) * str;
            float ml = sqrtf(mx * mx + mz * mz);
            if (ml > 0.05f) {
                if (ml > 1) { mx /= ml; mz /= ml; ml = 1; }
                px += mx * 6.0f * dt; pz += mz * 6.0f * dt; walk += 9.0f * dt;
                face = atan2f(mx, mz);
            }
            px = clampf(px, -120.f, 120.f); pz = clampf(pz, -120.f, 120.f);
            py = height(px, pz);

            atkCd -= dt; swing -= dt;
            if (atk && atkCd <= 0) {
                atkCd = 0.5f; swing = 0.2f;
                for (size_t i = 0; i < enemies.size(); i++) {
                    Enemy& e = enemies[i];
                    float dx = e.x - px, dz = e.z - pz, d = sqrtf(dx * dx + dz * dz);
                    if (d < 3.2f && (dx * sinf(face) + dz * cosf(face)) > -0.5f) {
                        e.hp -= (float)dmgVal(); e.flash = 0.2f;
                        if (d > 0.01f) { e.x += dx / d * 1.5f; e.z += dz / d * 1.5f; }
                    }
                }
            }

            // Gegner
            for (size_t i = 0; i < enemies.size(); i++) {
                Enemy& e = enemies[i];
                if (e.hp <= 0) {
                    addXp(10);
                    char b[48]; snprintf(b, sizeof b, "+10 EP");
                    msg(b);
                    float r = frand();
                    int drop = r < 0.35f ? IT_POTION : (r < 0.70f ? IT_BONE : (r < 0.85f ? IT_SHARD : -1));
                    if (drop >= 0 && pickups.size() < 40) { Pickup p; p.x = e.x; p.z = e.z; p.item = drop; pickups.push_back(p); }
                    respawn(e, px, pz);
                    continue;
                }
                e.flash -= dt;
                float dx = px - e.x, dz = pz - e.z, d = sqrtf(dx * dx + dz * dz);
                if (d < 25.0f && d > 0.8f) { e.x += dx / d * 2.2f * dt; e.z += dz / d * 2.2f * dt; }
                if (d < 1.4f) hp -= 20.0f * dt;
            }

            // Beute einsammeln
            for (size_t i = 0; i < pickups.size();) {
                float dx = pickups[i].x - px, dz = pickups[i].z - pz;
                if (dx * dx + dz * dz < 2.0f) {
                    items[pickups[i].item]++;
                    char b[48]; snprintf(b, sizeof b, "+1 %s", ITEM_NAME[pickups[i].item]);
                    msg(b);
                    pickups.erase(pickups.begin() + i);
                } else i++;
            }

            // Tod
            if (hp <= 0) {
                hp = (float)maxHp(); px = bx - 3.0f; pz = bz + 1.0f;
                resetEnemies(); msg("DU BIST GESTORBEN");
            }

            // Lagerfeuer: heilen & speichern
            float fdx = px - bx, fdz = pz - bz;
            bool inFire = fdx * fdx + fdz * fdz < 25.0f;
            hp = fminf(hp + (inFire ? 25.0f : 2.0f) * dt, (float)maxHp());
            if (inFire && !nearFire) { if (saveGame()) { hasSave = true; msg("SPIELSTAND GESPEICHERT"); } }
            nearFire = inFire;
        }

        // --- Kamera ---
        float camX, camY, camZ, tgX, tgY, tgZ;
        float by = height(bx, bz);
        if (state == ST_MENU) {
            float a = tm * 0.15f;
            camX = bx + sinf(a) * 11.0f; camZ = bz + cosf(a) * 11.0f;
            camY = height(camX, camZ) + 4.5f;
            tgX = bx; tgY = by + 1.3f; tgZ = bz;
        } else {
            camX = px + sinf(yaw) * 9.0f; camZ = pz + cosf(yaw) * 9.0f;
            camY = fmaxf(height(camX, camZ) + 1.5f, py + 5.0f);
            tgX = px; tgY = py + 1.2f; tgZ = pz;
        }
        int w, h; SDL_GL_GetDrawableSize(win, &w, &h);
        glViewport(0, 0, w, h);
        Mat vp = mul(perspective(1.0f, (float)w / h, 0.5f, 100.0f), lookAt(camX, camY, camZ, tgX, tgY, tgZ));

        // --- Zeichnen (3D) ---
        glUseProgram(prog);
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glEnableVertexAttribArray(2);
        glUniformMatrix4fv(uVP, 1, GL_FALSE, vp.m);
        glUniform3f(uCam, camX, camY, camZ);
        float flick = 0.85f + 0.15f * sinf(tm * 13.0f) * sinf(tm * 7.3f + 1.0f);
        glUniform4f(uFire, bx, by + 1.0f, bz, flick);
        glUniform1f(uFogMul, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        drawMesh(terrain, identity(), 0, 1, 1, 1);
        glUniform1f(uFogMul, 0.0f);
        drawMesh(stars, model(camX, camY, camZ, 0, 1, 1, 1), 0, 2.2f, 2.2f, 2.5f);
        glUniform1f(uFogMul, 1.0f);

        for (size_t i = 0; i < trees.size(); i++) {
            const Tree& t = trees[i];
            float ty = height(t.x, t.z);
            drawMesh(cube, model(t.x, ty + 1.0f, t.z, 0, 0.4f, 2.0f, 0.4f), 0, 0.25f, 0.15f, 0.08f);
            drawMesh(cube, model(t.x, ty + 2.6f, t.z, 0.6f, 2.4f, 1.8f, 2.4f), 0.6f, 0.05f, 0.14f, 0.11f);
            drawMesh(cube, model(t.x, ty + 4.2f, t.z, 1.2f, 1.7f, 1.8f, 1.7f), 1.2f, 0.05f, 0.14f, 0.11f);
            drawMesh(cube, model(t.x, ty + 5.8f, t.z, 1.8f, 1.0f, 1.8f, 1.0f), 1.8f, 0.05f, 0.14f, 0.11f);
        }

        // Lagerfeuer
        drawMesh(cube, model(bx, by + 0.15f, bz, 0.5f, 1.4f, 0.3f, 0.3f), 0.5f, 0.2f, 0.12f, 0.06f);
        drawMesh(cube, model(bx, by + 0.15f, bz, 2.1f, 1.4f, 0.3f, 0.3f), 2.1f, 0.2f, 0.12f, 0.06f);
        drawMesh(cube, model(bx, by + 0.7f, bz, tm * 2.0f, 0.5f, 0.9f * flick + 0.3f, 0.5f), tm * 2.0f, 1.0f, 0.55f, 0.1f);
        drawMesh(cube, model(bx, by + 1.2f, bz, -tm * 3.0f, 0.25f, 0.6f * flick, 0.25f), -tm * 3.0f, 1.0f, 0.8f, 0.3f);

        if (state == ST_MENU) {
            float kx = bx - 2.4f, kz = bz + 0.6f;
            drawKnight(cube, kx, kz, atan2f(bx - kx, bz - kz), 0.0f, false, 0.0f);
        } else {
            for (size_t i = 0; i < enemies.size(); i++) {
                const Enemy& e = enemies[i];
                float ey = height(e.x, e.z);
                float a = atan2f(px - e.x, pz - e.z), f = e.flash > 0 ? 1.0f : 0.0f;
                drawMesh(cube, model(e.x, ey + 0.9f, e.z, a, 1.0f, 1.8f, 1.0f), a, 0.35f + f * 0.6f, 0.05f + f * 0.6f, 0.08f + f * 0.6f);
            }
            for (size_t i = 0; i < pickups.size(); i++) {
                const Pickup& p = pickups[i];
                float yy = height(p.x, p.z) + 0.6f + 0.15f * sinf(tm * 3.0f + p.x);
                float cr = 1.4f, cg = 0.2f, cb = 0.25f;
                if (p.item == IT_BONE) { cr = 1.3f; cg = 1.25f; cb = 1.1f; }
                if (p.item == IT_SHARD) { cr = 1.0f; cg = 0.5f; cb = 1.5f; }
                drawMesh(cube, model(p.x, yy, p.z, tm * 2.0f, 0.35f, 0.35f, 0.35f), tm * 2.0f, cr, cg, cb);
            }
            float dmg = 1.0f - hp / (float)maxHp();
            float step = (fabsf(lx) + fabsf(ly) > 0.1f) ? sinf(walk) * 0.3f : 0.0f;
            drawKnight(cube, px, pz, face, step, swing > 0, dmg);
        }

        // --- Zeichnen (2D) ---
        begin2D();
        char buf[96];
        if (state == ST_MENU) {
            rect(0, 0, 320, 240, 0.0f, 0.0f, 0.03f, 0.40f);
            const char* title = "DARK REALM";
            float tw = textWidth(title, 4);
            text(title, (320 - tw) / 2 + 2, 42, 4, 0.0f, 0.0f, 0.0f, 0.8f);
            text(title, (320 - tw) / 2, 40, 4, 0.85f, 0.75f, 0.5f);
            const char* sub = "EIN DUESTERES ABENTEUER";
            text(sub, (320 - textWidth(sub, 1)) / 2, 80, 1, 0.6f, 0.6f, 0.7f);
            const char* labels[3] = {"NEUES SPIEL", "WEITERSPIELEN", "BEENDEN"};
            for (int i = 0; i < 3; i++) {
                bool enabled = !(i == 1 && !hasSave);
                float cr = 0.7f, cg = 0.7f, cb = 0.75f;
                if (!enabled) { cr = 0.3f; cg = 0.3f; cb = 0.33f; }
                if (i == menuSel && enabled) { cr = 1.0f; cg = 0.7f; cb = 0.25f; }
                float lw = textWidth(labels[i], 2);
                float ty = 115 + i * 26;
                if (i == menuSel) text(">", (320 - lw) / 2 - 16, ty, 2, cr, cg, cb);
                text(labels[i], (320 - lw) / 2, ty, 2, cr, cg, cb);
            }
            if (menuSel == 0 && hasSave) {
                const char* warn = "ACHTUNG: UEBERSCHREIBT DEN SPIELSTAND";
                text(warn, (320 - textWidth(warn, 1)) / 2, 198, 1, 0.9f, 0.3f, 0.3f);
            }
            const char* hint = "HOCH/RUNTER: WAEHLEN    A: OK";
            text(hint, (320 - textWidth(hint, 1)) / 2, 222, 1, 0.5f, 0.5f, 0.6f);
        } else {
            // Leisten oben links
            snprintf(buf, sizeof buf, "LV %d", level);
            text(buf, 8, 8, 1, 1.0f, 0.85f, 0.5f);
            rect(38, 7, 104, 11, 0, 0, 0, 0.6f);
            rect(40, 9, 100.0f * clampf(hp / (float)maxHp(), 0, 1), 7, 0.75f, 0.1f, 0.1f, 1.0f);
            snprintf(buf, sizeof buf, "%d/%d", (int)hp, maxHp());
            text(buf, 44, 9, 1, 1, 1, 1);
            rect(38, 21, 104, 7, 0, 0, 0, 0.6f);
            rect(40, 23, 100.0f * clampf((float)xp / (float)xpNeed(), 0, 1), 3, 0.3f, 0.5f, 0.95f, 1.0f);
            snprintf(buf, sizeof buf, "EP %d/%d", xp, xpNeed());
            text(buf, 146, 20, 1, 0.6f, 0.7f, 1.0f);
            snprintf(buf, sizeof buf, "X: HEILTRANK %d", items[IT_POTION]);
            text(buf, 8, 226, 1, 0.8f, 0.8f, 0.85f);
            for (size_t i = 0; i < msgs.size(); i++) {
                float al = clampf(msgs[i].second, 0.0f, 1.0f);
                text(msgs[i].first.c_str(), 8, 40.0f + i * 10.0f, 1, 1.0f, 1.0f, 0.85f, al);
            }
        }
        if (state == ST_INV) {
            rect(0, 0, 320, 240, 0, 0, 0, 0.45f);
            rect(48, 38, 224, 164, 0.5f, 0.4f, 0.25f, 1.0f);
            rect(50, 40, 220, 160, 0.04f, 0.04f, 0.06f, 0.96f);
            text("INVENTAR", 60, 48, 2, 0.9f, 0.8f, 0.55f);
            snprintf(buf, sizeof buf, "LEVEL %d   ANGRIFF %d", level, dmgVal());
            text(buf, 60, 70, 1, 0.65f, 0.65f, 0.75f);
            for (int i = 0; i < IT_COUNT; i++) {
                float ty = 92 + i * 14;
                bool sel = (i == invSel);
                float cr = sel ? 1.0f : 0.7f, cg = sel ? 0.7f : 0.7f, cb = sel ? 0.25f : 0.75f;
                if (sel) text(">", 58, ty, 1, cr, cg, cb);
                text(ITEM_NAME[i], 70, ty, 1, cr, cg, cb);
                snprintf(buf, sizeof buf, "X%d", items[i]);
                text(buf, 230, ty, 1, cr, cg, cb);
            }
            text(ITEM_DESC[invSel], 60, 150, 1, 0.6f, 0.75f, 0.6f);
            text("A: BENUTZEN    Y: ZURUECK", 60, 184, 1, 0.5f, 0.5f, 0.6f);
        }
        end2D();

        SDL_GL_SwapWindow(win);
    }

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
