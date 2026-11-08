// Dark Realm - 3D Dark-Fantasy Prototyp im PSP-Look (R36S / ArkOS)
// SDL2 + OpenGL ES 2.0, Vertexfarben, Gouraud-Licht, Nebel, Low-Poly
//
// Look: Nacht, Sterne, Kiefern, Lagerfeuer (heilt in der Naehe), Ritter mit Umhang
// Steuerung Handheld: Linker Stick = laufen, Rechter Stick = Kamera,
//                     A = Angriff, Start+Select = Beenden
// Steuerung PC:       WASD = laufen, Pfeil links/rechts = Kamera,
//                     Leertaste = Angriff, ESC = Beenden
#include <SDL.h>
#include <SDL_opengles2.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    float sx = -fz, sy = 0, sz = fx;                       // cross(f, up)
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

// ---------- Shader ----------
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

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, NULL, log); fprintf(stderr, "Shader-Fehler: %s\n", log); }
    return s;
}

static GLuint prog;
static GLint uVP, uModel, uRot, uTint, uCam, uFogCol, uFire, uFogMul;
static GLint aPos, aNor, aCol;

// ---------- Meshes ----------
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
    glVertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, 36, (void*)0);
    glVertexAttribPointer(aNor, 3, GL_FLOAT, GL_FALSE, 36, (void*)12);
    glVertexAttribPointer(aCol, 3, GL_FLOAT, GL_FALSE, 36, (void*)24);
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

struct Tree { float x, z; };
struct Enemy { float x, z, hp, flash; };

static void respawn(Enemy& e, float px, float pz) {
    float a = frand() * 6.2831f, d = 30.0f + frand() * 30.0f;
    e.x = fminf(fmaxf(px + cosf(a) * d, -120.f), 120.f);
    e.z = fminf(fmaxf(pz + sinf(a) * d, -120.f), 120.f);
    e.hp = 3; e.flash = 0;
}

static float axisVal(SDL_GameController* p, SDL_GameControllerAxis a) {
    float v = SDL_GameControllerGetAxis(p, a) / 32768.0f;
    return fabsf(v) < 0.2f ? 0.0f : v;
}
static float joyVal(SDL_Joystick* j, int a) {
    float v = SDL_JoystickGetAxis(j, a) / 32768.0f;
    return fabsf(v) < 0.2f ? 0.0f : v;
}

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

    prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, FS));
    glLinkProgram(prog);
    glUseProgram(prog);
    uVP = glGetUniformLocation(prog, "uVP"); uModel = glGetUniformLocation(prog, "uModel");
    uRot = glGetUniformLocation(prog, "uRot"); uTint = glGetUniformLocation(prog, "uTint");
    uCam = glGetUniformLocation(prog, "uCam"); uFogCol = glGetUniformLocation(prog, "uFogCol");
    uFire = glGetUniformLocation(prog, "uFire"); uFogMul = glGetUniformLocation(prog, "uFogMul");
    aPos = glGetAttribLocation(prog, "aPos"); aNor = glGetAttribLocation(prog, "aNor"); aCol = glGetAttribLocation(prog, "aCol");
    glEnableVertexAttribArray(aPos); glEnableVertexAttribArray(aNor); glEnableVertexAttribArray(aCol);

    const float fog[3] = {0.04f, 0.06f, 0.12f};
    glUniform3f(uFogCol, fog[0], fog[1], fog[2]);
    glClearColor(fog[0], fog[1], fog[2], 1);
    glEnable(GL_DEPTH_TEST);

    Mesh terrain = makeTerrain();
    Mesh cube = makeCube();
    Mesh stars = makeStars();

    srand(7);
    std::vector<Tree> trees;
    while (trees.size() < 80) {
        float x = (frand() - 0.5f) * 230, z = (frand() - 0.5f) * 230;
        if (x * x + z * z > 36) trees.push_back({x, z});
    }
    const float bx = 8.0f, bz = -6.0f;   // Lagerfeuer
    std::vector<Enemy> enemies(6);
    float px = 0, pz = 0, face = 0, yaw = 0, hp = 100, atkCd = 0, swing = 0, tm = 0, walk = 0;
    for (auto& e : enemies) respawn(e, px, pz);

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
        if (pad) {
            lx = axisVal(pad, SDL_CONTROLLER_AXIS_LEFTX); ly = axisVal(pad, SDL_CONTROLLER_AXIS_LEFTY);
            rx = axisVal(pad, SDL_CONTROLLER_AXIS_RIGHTX);
            atk = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A);
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START) &&
                SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK)) run = false;
        } else if (joy) {
            lx = joyVal(joy, 0); ly = joyVal(joy, 1); rx = joyVal(joy, 2);
            atk = SDL_JoystickGetButton(joy, 0);
        }
        const Uint8* k = SDL_GetKeyboardState(NULL);
        if (k[SDL_SCANCODE_A]) lx -= 1; if (k[SDL_SCANCODE_D]) lx += 1;
        if (k[SDL_SCANCODE_W]) ly -= 1; if (k[SDL_SCANCODE_S]) ly += 1;
        if (k[SDL_SCANCODE_LEFT]) rx -= 1; if (k[SDL_SCANCODE_RIGHT]) rx += 1;
        if (k[SDL_SCANCODE_SPACE]) atk = true;

        // --- Spieler ---
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
        px = fminf(fmaxf(px, -120.f), 120.f); pz = fminf(fmaxf(pz, -120.f), 120.f);
        float py = height(px, pz);

        atkCd -= dt; swing -= dt;
        if (atk && atkCd <= 0) {
            atkCd = 0.5f; swing = 0.2f;
            for (auto& e : enemies) {
                float dx = e.x - px, dz = e.z - pz, d = sqrtf(dx * dx + dz * dz);
                if (d < 3.2f && (dx * sinf(face) + dz * cosf(face)) > -0.5f) {
                    e.hp -= 1; e.flash = 0.2f;
                    if (d > 0.01f) { e.x += dx / d * 1.5f; e.z += dz / d * 1.5f; }
                }
            }
        }

        // --- Gegner ---
        for (auto& e : enemies) {
            if (e.hp <= 0) { respawn(e, px, pz); continue; }
            e.flash -= dt;
            float dx = px - e.x, dz = pz - e.z, d = sqrtf(dx * dx + dz * dz);
            if (d < 25.0f && d > 0.8f) { e.x += dx / d * 2.2f * dt; e.z += dz / d * 2.2f * dt; }
            if (d < 1.4f) hp -= 20.0f * dt;
        }
        if (hp <= 0) { hp = 100; px = pz = 0; for (auto& e : enemies) respawn(e, px, pz); }
        float fdx = px - bx, fdz = pz - bz;
        hp = fminf(hp + (fdx * fdx + fdz * fdz < 25.0f ? 25.0f : 2.0f) * dt, 100.0f);

        // --- Kamera ---
        float cd = 9.0f, ch = 5.0f;
        float camX = px + sinf(yaw) * cd, camZ = pz + cosf(yaw) * cd;
        float camY = fmaxf(height(camX, camZ) + 1.5f, py + ch);
        int w, h; SDL_GL_GetDrawableSize(win, &w, &h);
        glViewport(0, 0, w, h);
        Mat vp = mul(perspective(1.0f, (float)w / h, 0.5f, 100.0f), lookAt(camX, camY, camZ, px, py + 1.2f, pz));
        glUniformMatrix4fv(uVP, 1, GL_FALSE, vp.m);
        glUniform3f(uCam, camX, camY, camZ);
        float flick = 0.85f + 0.15f * sinf(tm * 13.0f) * sinf(tm * 7.3f + 1.0f);
        float by = height(bx, bz);
        glUniform4f(uFire, bx, by + 1.0f, bz, flick);
        glUniform1f(uFogMul, 1.0f);

        // --- Zeichnen ---
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        drawMesh(terrain, identity(), 0, 1, 1, 1);
        glUniform1f(uFogMul, 0.0f);
        drawMesh(stars, model(camX, camY, camZ, 0, 1, 1, 1), 0, 2.2f, 2.2f, 2.5f);
        glUniform1f(uFogMul, 1.0f);

        for (auto& t : trees) {
            float ty = height(t.x, t.z);
            drawMesh(cube, model(t.x, ty + 1.0f, t.z, 0, 0.4f, 2.0f, 0.4f), 0, 0.25f, 0.15f, 0.08f);
            drawMesh(cube, model(t.x, ty + 2.6f, t.z, 0.6f, 2.4f, 1.8f, 2.4f), 0.6f, 0.05f, 0.14f, 0.11f);
            drawMesh(cube, model(t.x, ty + 4.2f, t.z, 1.2f, 1.7f, 1.8f, 1.7f), 1.2f, 0.05f, 0.14f, 0.11f);
            drawMesh(cube, model(t.x, ty + 5.8f, t.z, 1.8f, 1.0f, 1.8f, 1.0f), 1.8f, 0.05f, 0.14f, 0.11f);
        }
        for (auto& e : enemies) {
            float ey = height(e.x, e.z);
            float a = atan2f(px - e.x, pz - e.z), f = e.flash > 0 ? 1.0f : 0.0f;
            drawMesh(cube, model(e.x, ey + 0.9f, e.z, a, 1.0f, 1.8f, 1.0f), a, 0.35f + f * 0.6f, 0.05f + f * 0.6f, 0.08f + f * 0.6f);
        }
        // Lagerfeuer
        drawMesh(cube, model(bx, by + 0.15f, bz, 0.5f, 1.4f, 0.3f, 0.3f), 0.5f, 0.2f, 0.12f, 0.06f);
        drawMesh(cube, model(bx, by + 0.15f, bz, 2.1f, 1.4f, 0.3f, 0.3f), 2.1f, 0.2f, 0.12f, 0.06f);
        drawMesh(cube, model(bx, by + 0.7f, bz, tm * 2.0f, 0.5f, 0.9f * flick + 0.3f, 0.5f), tm * 2.0f, 1.0f, 0.55f, 0.1f);
        drawMesh(cube, model(bx, by + 1.2f, bz, -tm * 3.0f, 0.25f, 0.6f * flick, 0.25f), -tm * 3.0f, 1.0f, 0.8f, 0.3f);

        // Ritter
        float dmg = 1.0f - hp / 100.0f;
        float step = ml > 0.05f ? sinf(walk) * 0.3f : 0.0f;
        float cf = cosf(face), sf = sinf(face);
        auto part = [&](float lx, float ly, float lz, float sx, float sy, float sz, float r, float g, float b) {
            float wx = px + lx * cf + lz * sf, wz = pz - lx * sf + lz * cf;
            drawMesh(cube, model(wx, py + ly, wz, face, sx, sy, sz), face,
                     r * (1.0f + dmg * 0.8f), g * (1.0f - dmg * 0.6f), b * (1.0f - dmg * 0.6f));
        };
        part(-0.22f, 0.45f, step, 0.3f, 0.9f, 0.32f, 0.33f, 0.35f, 0.40f);          // Beine
        part( 0.22f, 0.45f, -step, 0.3f, 0.9f, 0.32f, 0.33f, 0.35f, 0.40f);
        part(0, 1.35f, 0, 0.9f, 0.9f, 0.55f, 0.50f, 0.53f, 0.58f);                 // Brustpanzer
        part(-0.65f, 1.72f, 0, 0.42f, 0.3f, 0.55f, 0.58f, 0.61f, 0.66f);            // Schulterplatten
        part( 0.65f, 1.72f, 0, 0.42f, 0.3f, 0.55f, 0.58f, 0.61f, 0.66f);
        part(-0.65f, 1.15f, -step * 0.8f, 0.26f, 0.8f, 0.3f, 0.45f, 0.48f, 0.53f); // Arme
        part( 0.65f, 1.15f, step * 0.8f, 0.26f, 0.8f, 0.3f, 0.45f, 0.48f, 0.53f);
        part(0, 2.05f, 0, 0.52f, 0.55f, 0.52f, 0.55f, 0.58f, 0.63f);                // Helm
        part(0, 2.05f, 0.27f, 0.34f, 0.07f, 0.06f, 0.02f, 0.02f, 0.03f);           // Visier
        part(0, 1.4f, -0.36f, 0.95f, 1.5f, 0.1f, 0.5f, 0.32f, 0.12f);              // Umhang
        part(0.85f, 1.2f, 0.2f, 0.1f, 0.85f, 0.6f, 0.28f, 0.22f, 0.18f);           // Schild
        if (swing > 0) part(-0.65f, 1.3f, 1.3f, 0.12f, 0.12f, 1.9f, 0.85f, 0.85f, 0.95f); // Schwerthieb
        else part(-0.7f, 1.2f, 0.5f, 0.1f, 0.1f, 1.3f, 0.7f, 0.7f, 0.8f);                 // Schwert (Ruhe)

        SDL_GL_SwapWindow(win);
    }

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
