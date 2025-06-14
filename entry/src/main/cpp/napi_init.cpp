#include "napi/native_api.h"
#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <assert.h>
#include <cstdint>
#include <deque>
#include <fcntl.h>
#include <map>
#include <native_window/external_window.h>
#include <poll.h>
#include <pty.h>
#include <set>
#include <stdio.h>
#include <string>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "hilog/log.h"
#undef LOG_TAG
#define LOG_TAG "testTag"

// docs for escape codes:
// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
// https://vt100.net/docs/vt220-rm/chapter4.html
// https://espterm.github.io/docs/VT100%20escape%20codes.html
// https://ecma-international.org/wp-content/uploads/ECMA-48_5th_edition_june_1991.pdf
// https://xtermjs.org/docs/api/vtfeatures/

static int fd = -1;

enum weight {
    regular = 0,
    bold = 1,
    NUM_WEIGHT,
};

// maintain terminal status
struct style {
    weight weight = regular;
    // foreground color
    float fg_red = 0.0;
    float fg_green = 0.0;
    float fg_blue = 0.0;
    // background color
    float bg_red = 1.0;
    float bg_green = 1.0;
    float bg_blue = 1.0;
};
struct term_char {
    uint32_t ch = ' ';
    style style;
};

static int MAX_HISTORY_LINES = 5000;
static std::deque<std::vector<term_char>> history;
static std::vector<std::vector<term_char>> terminal;
static int row = 0;
static int col = 0;
enum escape_states {
    state_idle,
    state_esc,
    state_csi,
    state_osc,
    state_dcs,
};
static escape_states escape_state = state_idle;
enum utf8_states {
    state_initial,
    state_2byte_2,        // expected 2nd byte of 2-byte sequence
    state_3byte_2_e0,     // expected 2nd byte of 3-byte sequence starting with 0xe0
    state_3byte_2_non_e0, // expected 2nd byte of 3-byte sequence starting with non-0xe0
    state_3byte_3,        // expected 3rd byte of 3-byte sequence
    state_4byte_2_f0,     // expected 2nd byte of 4-byte sequence starting with 0xf0
    state_4byte_2_f1_f3,  // expected 2nd byte of 4-byte sequence starting with 0xf1 to 0xf3
    state_4byte_2_f4,     // expected 2nd byte of 4-byte sequence starting with 0xf4
    state_4byte_3,        // expected 3rd byte of 4-byte sequence
    state_4byte_4,        // expected 4th byte of 4-byte sequence
};
static utf8_states utf8_state = state_initial;
static uint32_t current_utf8 = 0;
static std::string escape_buffer;
static style current_style;
static int width = 0;
static int height = 0;
static bool show_cursor = true;
static GLint surface_location = -1;
static GLint render_pass_location = -1;
static int font_height = 48;
static int font_width = 24;
static int max_font_width = 48;
static int baseline_height = 10;
static int term_col = 80;
static int term_row = 24;
static float scroll_offset = 0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

extern "C" int mkdir(const char *pathname, mode_t mode);
static napi_value Run(napi_env env, napi_callback_info info) {
    if (fd != -1) {
        return nullptr;
    }

    terminal.resize(term_row);
    for (int i = 0; i < term_row; i++) {
        terminal[i].resize(term_col);
    }

    struct winsize ws = {};
    ws.ws_col = term_col;
    ws.ws_row = term_row;

    int pid = forkpty(&fd, nullptr, nullptr, &ws);
    if (!pid) {
        // override HOME to /storage/Users/currentUser since it is writable
        const char *home = "/storage/Users/currentUser";
        setenv("HOME", home, 1);
        setenv("PWD", home, 1);
        chdir(home);
        execl("/data/app/bin/bash", "/data/app/bin/bash", nullptr);
    }

    int res = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    assert(res == 0);
    return nullptr;
}

std::vector<std::string> splitString(const std::string &str, const std::string &delimiter) {
    std::vector<std::string> result;
    size_t start = 0;
    size_t end = str.find(delimiter);
    while (end != std::string::npos) {
        result.push_back(str.substr(start, end - start));
        start = end + delimiter.length();
        end = str.find(delimiter, start);
    }
    result.push_back(str.substr(start));
    return result;
}

static napi_value Send(napi_env env, napi_callback_info info) {
    if (fd == -1) {
        return nullptr;
    }

    // reset scroll offset to bottom
    scroll_offset = 0.0;

    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    void *data;
    size_t length;
    napi_status ret = napi_get_arraybuffer_info(env, args[0], &data, &length);
    assert(ret == napi_ok);
    int written = 0;
    while (written < length) {
        int size = write(fd, (uint8_t *)data + written, length - written);
        assert(size >= 0);
        written += size;
    }
    return nullptr;
}


// https://learnopengl.com/In-Practice/Text-Rendering
struct ivec2 {
    int x;
    int y;

    ivec2(int x, int y) {
        this->x = x;
        this->y = y;
    }
    ivec2() { this->x = this->y = 0; }
};

struct character {
    // location within the large texture
    float left;
    float right;
    float top;
    float bottom;
    // x, y offset from origin for bearing etc.
    int xoff;
    int yoff;
    // glyph size
    int width;
    int height;
};

// record info for each character
// map from (codepoint, font weight) to character
static std::map<std::pair<uint32_t, enum weight>, struct character> characters;
// code points to load from the font
static std::set<uint32_t> codepoints_to_load;
// do we need to reload font due to missing glyphs?
static bool need_reload_font = false;

// id of texture for glyphs
static GLuint texture_id;

// load font
// texture contains all glyphs of all weights:
// fixed width of max_font_width, variable height based on face->glyph->bitmap.rows
// glyph goes in vertical, possibly not filling the whole row space:
//    0.0       1.0
// 0.0 +------+--+
//     | 0x00 |  |
// 0.5 +------+--+
//     | 0x01    |
// 1.0 +------+--+
static void LoadFont() {
    need_reload_font = false;

    FT_Library ft;
    FT_Error err = FT_Init_FreeType(&ft);
    assert(err == 0);

    std::vector<std::pair<const char *, weight>> fonts = {
        {"/data/storage/el2/base/haps/entry/files/Inconsolata-Regular.ttf", weight::regular},
        {"/data/storage/el2/base/haps/entry/files/Inconsolata-Bold.ttf", weight::bold},
    };

    // save glyph for all characters of all weights
    // only one channel
    std::vector<uint8_t> bitmap;
    int row_stride = max_font_width;
    int bitmap_height = 0;

    for (auto pair : fonts) {
        const char *font = pair.first;
        weight weight = pair.second;

        FT_Face face;
        err = FT_New_Face(ft, font, 0, &face);
        assert(err == 0);
        FT_Set_Pixel_Sizes(face, 0, font_height);
        // Note: in 26.6 fractional pixel format
        OH_LOG_INFO(LOG_APP,
                    "Ascender: %{public}d Descender: %{public}d Height: %{public}d XMin: %{public}ld XMax: %{public}ld "
                    "YMin: %{public}ld YMax: %{public}ld XScale: %{public}ld YScale: %{public}ld",
                    face->ascender, face->descender, face->height, face->bbox.xMin, face->bbox.xMax, face->bbox.yMin,
                    face->bbox.yMax, face->size->metrics.x_scale, face->size->metrics.y_scale);

        for (uint32_t c : codepoints_to_load) {
            // load character glyph
            assert(FT_Load_Char(face, c, FT_LOAD_RENDER) == 0);

            OH_LOG_INFO(LOG_APP,
                        "Weight: %{public}d Char: %{public}c(%{public}d) Glyph: %{public}d %{public}d Left: "
                        "%{public}d "
                        "Top: %{public}d "
                        "Advance: %{public}ld",
                        weight, c, c, face->glyph->bitmap.width, face->glyph->bitmap.rows, face->glyph->bitmap_left,
                        face->glyph->bitmap_top, face->glyph->advance.x);

            // copy to bitmap
            int old_bitmap_height = bitmap_height;
            int new_bitmap_height = bitmap_height + face->glyph->bitmap.rows;
            bitmap.resize(row_stride * new_bitmap_height);
            bitmap_height = new_bitmap_height;

            assert(face->glyph->bitmap.width <= row_stride);
            for (int i = 0; i < face->glyph->bitmap.rows; i++) {
                for (int j = 0; j < face->glyph->bitmap.width; j++) {
                    // compute offset in the large texture
                    int off = old_bitmap_height * row_stride;
                    bitmap[i * row_stride + j + off] = face->glyph->bitmap.buffer[i * face->glyph->bitmap.width + j];
                }
            }

            // compute location within the texture
            // first pass: store pixels
            character character = {
                .left = 0,
                .right = (float)face->glyph->bitmap.width - 1,
                .top = (float)old_bitmap_height,
                .bottom = (float)new_bitmap_height - 1,
                .xoff = face->glyph->bitmap_left,
                .yoff = (int)(baseline_height + face->glyph->bitmap_top - face->glyph->bitmap.rows),
                .width = (int)face->glyph->bitmap.width,
                .height = (int)face->glyph->bitmap.rows,
            };
            characters[{c, weight}] = character;
        }


        FT_Done_Face(face);
    }

    FT_Done_FreeType(ft);

    // now bitmap contains all glyphs
    // second pass: convert pixels to uv coordinates
    for (auto &pair : characters) {
        pair.second.left /= row_stride - 1;
        pair.second.right /= row_stride - 1;
        pair.second.top /= bitmap_height - 1;
        pair.second.bottom /= bitmap_height - 1;
    }

    // disable byte-alignment restriction
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // generate texture
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, row_stride, bitmap_height, 0, GL_RED, GL_UNSIGNED_BYTE, bitmap.data());

    // set texture options
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

static EGLDisplay egl_display;
static EGLSurface egl_surface;
static EGLContext egl_context;
static GLuint program_id;
static GLuint vertex_array;
// vec4 vertex
static GLuint vertex_buffer;
// vec3 textColor
static GLuint text_color_buffer;
// vec3 backGroundColor
static GLuint background_color_buffer;

static void Draw() {
    // clear buffer
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // update surface size
    pthread_mutex_lock(&lock);
    glUniform2f(surface_location, width, height);
    glViewport(0, 0, width, height);

    // set texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_id);

    // bind our vertex array
    glBindVertexArray(vertex_array);

    int max_lines = height / font_height;
    // vec4 vertex
    static std::vector<GLfloat> vertex_pass0_data;
    static std::vector<GLfloat> vertex_pass1_data;
    // vec3 textColor
    static std::vector<GLfloat> text_color_data;
    // vec3 backgroundColor
    static std::vector<GLfloat> background_color_data;

    vertex_pass0_data.clear();
    vertex_pass0_data.reserve(row * col * 24);
    vertex_pass1_data.clear();
    vertex_pass1_data.reserve(row * col * 24);
    text_color_data.clear();
    text_color_data.reserve(row * col * 18);
    background_color_data.clear();
    background_color_data.reserve(row * col * 18);

    // ensure at least one line shown, for very large scroll_offset
    int scroll_rows = scroll_offset / font_height;
    if ((int)history.size() + max_lines - 1 - scroll_rows < 0) {
        scroll_offset = ((int)history.size() + max_lines - 1) * font_height;
        scroll_rows = scroll_offset / font_height;
    }

    for (int i = 0; i < max_lines; i++) {
        // (height - font_height) is terminal[0] when scroll_offset is zero
        float x = 0.0;
        float y = height - (i + 1) * font_height;
        int i_row = i - scroll_rows;
        std::vector<term_char> ch;
        if (i_row >= 0 && i_row < term_row) {
            ch = terminal[i_row];
        } else if (i_row < 0 && (int)history.size() + i_row >= 0) {
            ch = history[history.size() + i_row];
        } else {
            continue;
        }

        int cur_col = 0;
        for (auto c : ch) {
            uint32_t codepoint = c.ch;
            auto key = std::pair<uint32_t, enum weight>(c.ch, c.style.weight);
            auto it = characters.find(key);
            if (it == characters.end()) {
                // reload font to locate it
                OH_LOG_WARN(LOG_APP, "Missing character: %{public}d of weight %{public}d", c.ch, c.style.weight);
                need_reload_font = true;
                codepoints_to_load.insert(c.ch);

                // we don't have the character, fallback to space
                it = characters.find(std::pair<uint32_t, enum weight>(' ', c.style.weight));
                assert(it != characters.end());
            }

            character ch = it->second;
            float xpos = x;
            float ypos = y;
            float w = font_width;
            float h = font_height;

            // 1-2
            // | |
            // 3-4
            // (xpos    , ypos + h): 1
            // (xpos + w, ypos + h): 2
            // (xpos    , ypos    ): 3
            // (xpos + w, ypos    ): 4

            // pass 0: draw background
            GLfloat g_vertex_pass0_data[24] = {// first triangle: 1->3->4
                                               xpos, ypos + h, 0.0, 0.0, xpos, ypos, 0.0, 0.0, xpos + w, ypos, 0.0, 0.0,
                                               // second triangle: 1->4->2
                                               xpos, ypos + h, 0.0, 0.0, xpos + w, ypos, 0.0, 0.0, xpos + w, ypos + h,
                                               0.0, 0.0};
            vertex_pass0_data.insert(vertex_pass0_data.end(), &g_vertex_pass0_data[0], &g_vertex_pass0_data[24]);

            // pass 1: draw text
            xpos = x + ch.xoff;
            ypos = y + ch.yoff;
            w = ch.width;
            h = ch.height;
            GLfloat g_vertex_pass1_data[24] = {// first triangle: 1->3->4
                                               xpos, ypos + h, ch.left, ch.top, xpos, ypos, ch.left, ch.bottom,
                                               xpos + w, ypos, ch.right, ch.bottom,
                                               // second triangle: 1->4->2
                                               xpos, ypos + h, ch.left, ch.top, xpos + w, ypos, ch.right, ch.bottom,
                                               xpos + w, ypos + h, ch.right, ch.top};
            vertex_pass1_data.insert(vertex_pass1_data.end(), &g_vertex_pass1_data[0], &g_vertex_pass1_data[24]);

            GLfloat g_text_color_buffer_data[18];
            GLfloat g_background_color_buffer_data[18];

            if (i_row == row && cur_col == col && show_cursor) {
                // cursor
                for (int i = 0; i < 6; i++) {
                    g_text_color_buffer_data[i * 3 + 0] = 1.0 - c.style.fg_red;
                    g_text_color_buffer_data[i * 3 + 1] = 1.0 - c.style.fg_green;
                    g_text_color_buffer_data[i * 3 + 2] = 1.0 - c.style.fg_blue;
                    g_background_color_buffer_data[i * 3 + 0] = 1.0 - c.style.bg_red;
                    g_background_color_buffer_data[i * 3 + 1] = 1.0 - c.style.bg_green;
                    g_background_color_buffer_data[i * 3 + 2] = 1.0 - c.style.bg_blue;
                }
            } else {
                for (int i = 0; i < 6; i++) {
                    g_text_color_buffer_data[i * 3 + 0] = c.style.fg_red;
                    g_text_color_buffer_data[i * 3 + 1] = c.style.fg_green;
                    g_text_color_buffer_data[i * 3 + 2] = c.style.fg_blue;
                    g_background_color_buffer_data[i * 3 + 0] = c.style.bg_red;
                    g_background_color_buffer_data[i * 3 + 1] = c.style.bg_green;
                    g_background_color_buffer_data[i * 3 + 2] = c.style.bg_blue;
                }
            }
            text_color_data.insert(text_color_data.end(), &g_text_color_buffer_data[0], &g_text_color_buffer_data[18]);
            background_color_data.insert(background_color_data.end(), &g_background_color_buffer_data[0],
                                         &g_background_color_buffer_data[18]);

            x += font_width;
            cur_col++;
        }
    }
    pthread_mutex_unlock(&lock);

    // draw in two pass
    glBindBuffer(GL_ARRAY_BUFFER, text_color_buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * text_color_data.size(), text_color_data.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, background_color_buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * background_color_data.size(), background_color_data.data(),
                 GL_STREAM_DRAW);

    // first pass
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * vertex_pass0_data.size(), vertex_pass0_data.data(), GL_STREAM_DRAW);
    glUniform1i(render_pass_location, 0);
    glDrawArrays(GL_TRIANGLES, 0, vertex_pass0_data.size() / 4);

    // second pass
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * vertex_pass1_data.size(), vertex_pass1_data.data(), GL_STREAM_DRAW);
    glUniform1i(render_pass_location, 1);
    glDrawArrays(GL_TRIANGLES, 0, vertex_pass1_data.size() / 4);

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glFlush();
    glFinish();
    eglSwapBuffers(egl_display, egl_surface);
}

static void DropFirstRowIfOverflow() {
    if (row == term_row) {
        // drop first row
        history.push_back(terminal[0]);
        terminal.erase(terminal.begin());
        terminal.resize(term_row);
        terminal[term_row - 1].resize(term_col);
        row--;
        while (history.size() > MAX_HISTORY_LINES) {
            history.pop_front();
        }
    }
}

#define clamp_row()                                                                                                    \
    do {                                                                                                               \
        if (row < 0) {                                                                                                 \
            row = 0;                                                                                                   \
        } else if (row > term_row - 1) {                                                                               \
            row = term_row - 1;                                                                                        \
        }                                                                                                              \
    } while (0);

#define clamp_col()                                                                                                    \
    do {                                                                                                               \
        if (col < 0) {                                                                                                 \
            col = 0;                                                                                                   \
        } else if (col > term_col - 1) {                                                                               \
            col = term_col - 1;                                                                                        \
        }                                                                                                              \
    } while (0);

// CAUTION: clobbers temp
#define read_int_or_default(def)                                                                                       \
    (temp = 0, (escape_buffer != "" ? sscanf(escape_buffer.c_str(), "%d", &temp) : temp = (def)), temp)

static void *RenderWorker(void *) {
    pthread_setname_np(pthread_self(), "render worker");

    eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);

    // build vertex and fragment shader
    GLuint vertex_shader_id = glCreateShader(GL_VERTEX_SHADER);
    char const *vertex_source = "#version 320 es\n"
                                "\n"
                                "in vec4 vertex;\n"
                                "in vec3 textColor;\n"
                                "in vec3 backgroundColor;\n"
                                "out vec2 texCoords;\n"
                                "out vec3 fragTextColor;\n"
                                "out vec3 fragBackgroundColor;\n"
                                "uniform vec2 surface;\n"
                                "void main() {\n"
                                "  gl_Position.x = vertex.x / surface.x * 2.0f - 1.0f;\n"
                                "  gl_Position.y = vertex.y / surface.y * 2.0f - 1.0f;\n"
                                "  gl_Position.z = 0.0;\n"
                                "  gl_Position.w = 1.0;\n"
                                "  texCoords = vertex.zw;\n"
                                "  fragTextColor = textColor;\n"
                                "  fragBackgroundColor = backgroundColor;\n"
                                "}";
    glShaderSource(vertex_shader_id, 1, &vertex_source, NULL);
    glCompileShader(vertex_shader_id);

    int info_log_length;
    glGetShaderiv(vertex_shader_id, GL_INFO_LOG_LENGTH, &info_log_length);
    if (info_log_length > 0) {
        std::vector<char> vertex_shader_error_message(info_log_length + 1);
        glGetShaderInfoLog(vertex_shader_id, info_log_length, NULL, &vertex_shader_error_message[0]);
        OH_LOG_ERROR(LOG_APP, "Failed to build vertex shader: %{public}s", &vertex_shader_error_message[0]);
    }

    GLuint fragment_shader_id = glCreateShader(GL_FRAGMENT_SHADER);
    char const *fragment_source = "#version 320 es\n"
                                  "\n"
                                  "precision lowp float;\n"
                                  "in vec2 texCoords;\n"
                                  "in vec3 fragTextColor;\n"
                                  "in vec3 fragBackgroundColor;\n"
                                  "out vec4 color;\n"
                                  "uniform sampler2D text;\n"
                                  "uniform int renderPass;\n"
                                  "void main() {\n"
                                  "  if (renderPass == 0) {\n"
                                  "    color = vec4(fragBackgroundColor, 1.0);\n"
                                  "  } else {\n"
                                  "    float alpha = texture(text, texCoords).r;\n"
                                  "    color = vec4(fragTextColor, 1.0) * alpha;\n"
                                  "  }\n"
                                  "}";
    // blending is done by opengl (GL_ONE + GL_ONE_MINUS_SRC_ALPHA):
    // final = src * 1 + dest * (1 - src.a)
    // first pass: src = (fragBackgroundColor, 1.0), dest = (1.0, 1.0, 1.0, 1.0), final = (fragBackgroundColor, 1.0)
    // second pass: src = (fragTextColor * alpha, alpha), dest = (fragBackgroundColor, 1.0), final = (fragTextColor *
    // alpha + fragBackgroundColor * (1 - alpha), 1.0)
    glShaderSource(fragment_shader_id, 1, &fragment_source, NULL);
    glCompileShader(fragment_shader_id);

    glGetShaderiv(fragment_shader_id, GL_INFO_LOG_LENGTH, &info_log_length);
    if (info_log_length > 0) {
        std::vector<char> fragment_shader_error_message(info_log_length + 1);
        glGetShaderInfoLog(fragment_shader_id, info_log_length, NULL, &fragment_shader_error_message[0]);
        OH_LOG_ERROR(LOG_APP, "Failed to build fragment shader: %{public}s", &fragment_shader_error_message[0]);
    }

    GLuint program_id = glCreateProgram();
    glAttachShader(program_id, vertex_shader_id);
    glAttachShader(program_id, fragment_shader_id);
    glLinkProgram(program_id);

    glGetProgramiv(program_id, GL_INFO_LOG_LENGTH, &info_log_length);
    if (info_log_length > 0) {
        std::vector<char> link_program_error_message(info_log_length + 1);
        glGetProgramInfoLog(program_id, info_log_length, NULL, &link_program_error_message[0]);
        OH_LOG_ERROR(LOG_APP, "Failed to link program: %{public}s", &link_program_error_message[0]);
    }

    surface_location = glGetUniformLocation(program_id, "surface");
    assert(surface_location != -1);

    render_pass_location = glGetUniformLocation(program_id, "renderPass");
    assert(render_pass_location != -1);

    glUseProgram(program_id);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // load font from ttf for the initial characters
    glGenTextures(1, &texture_id);
    // load common characters initially
    for (uint32_t i = 0; i < 128; i++) {
        codepoints_to_load.insert(i);
    }
    LoadFont();

    // create buffers for drawing
    glGenVertexArrays(1, &vertex_array);
    glBindVertexArray(vertex_array);

    // vec4 vertex
    glGenBuffers(1, &vertex_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
    GLint vertex_location = glGetAttribLocation(program_id, "vertex");
    assert(vertex_location != -1);
    glEnableVertexAttribArray(vertex_location);
    glVertexAttribPointer(vertex_location,   // attribute 0
                          4,                 // size
                          GL_FLOAT,          // type
                          GL_FALSE,          // normalized?
                          4 * sizeof(float), // stride
                          (void *)0          // array buffer offset
    );

    // vec3 textColor
    glGenBuffers(1, &text_color_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, text_color_buffer);
    GLint text_color_location = glGetAttribLocation(program_id, "textColor");
    assert(text_color_location != -1);
    glEnableVertexAttribArray(text_color_location);
    glVertexAttribPointer(text_color_location, // attribute 0
                          3,                   // size
                          GL_FLOAT,            // type
                          GL_FALSE,            // normalized?
                          3 * sizeof(float),   // stride
                          (void *)0            // array buffer offset
    );

    // vec3 backgroundColor
    glGenBuffers(1, &background_color_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, background_color_buffer);
    GLint background_color_location = glGetAttribLocation(program_id, "backgroundColor");
    assert(background_color_location != -1);
    glEnableVertexAttribArray(background_color_location);
    glVertexAttribPointer(background_color_location, // attribute 0
                          3,                         // size
                          GL_FLOAT,                  // type
                          GL_FALSE,                  // normalized?
                          3 * sizeof(float),         // stride
                          (void *)0                  // array buffer offset
    );

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    uint64_t last_redraw_msec = tv.tv_sec * 1000 + tv.tv_usec / 1000;
    uint64_t last_fps_msec = last_redraw_msec;
    Draw();
    int fps = 0;
    std::vector<uint64_t> time;
    while (1) {
        gettimeofday(&tv, nullptr);
        uint64_t now_msec = tv.tv_sec * 1000 + tv.tv_usec / 1000;

        // even if we call faster than system settings (60Hz/120Hz), it does not get faster
        // 120 Hz, 8ms
        uint64_t deadline = last_redraw_msec + 8;
        if (now_msec < deadline) {
            usleep((deadline - now_msec) * 1000);
        }

        // redraw
        gettimeofday(&tv, nullptr);
        now_msec = tv.tv_sec * 1000 + tv.tv_usec / 1000;
        last_redraw_msec = now_msec;
        Draw();

        gettimeofday(&tv, nullptr);
        uint64_t msec = tv.tv_sec * 1000 + tv.tv_usec / 1000;
        time.push_back(msec - now_msec);

        fps++;

        // report fps
        if (now_msec - last_fps_msec > 1000) {
            last_fps_msec = now_msec;
            uint64_t sum = 0;
            for (auto t : time) {
                sum += t;
            }
            OH_LOG_INFO(LOG_APP, "FPS: %{public}d, %{public}ld ms per draw", fps, sum / fps);
            fps = 0;
            time.clear();
        }

        if (need_reload_font) {
            LoadFont();
        }
    }
}

static void InsertUtf8(uint32_t codepoint) {
    assert(row >= 0 && row < term_row);
    assert(col >= 0 && col < term_col);
    terminal[row][col].ch = codepoint;
    terminal[row][col].style = current_style;
    col++;
    if (col == term_col) {
        col = 0;
        row++;
        DropFirstRowIfOverflow();
    }
}

static void *TerminalWorker(void *) {
    pthread_setname_np(pthread_self(), "terminal worker");

    int temp = 0;
    // poll from fd, and render
    struct timeval tv;
    while (1) {
        struct pollfd fds[1];
        fds[0].fd = fd;
        fds[0].events = POLLIN;
        int res = poll(fds, 1, 1000);

        uint8_t buffer[1024];
        if (res > 0) {
            ssize_t r = read(fd, buffer, sizeof(buffer) - 1);
            if (r > 0) {
                // pretty print
                std::string hex;
                for (int i = 0; i < r; i++) {
                    if (buffer[i] >= 127 || buffer[i] < 32) {
                        char temp[8];
                        snprintf(temp, sizeof(temp), "\\x%02x", buffer[i]);
                        hex += temp;
                    } else {
                        hex += (char)buffer[i];
                    }
                }
                OH_LOG_INFO(LOG_APP, "Got: %{public}s", hex.c_str());

                // parse output
                pthread_mutex_lock(&lock);
                for (int i = 0; i < r; i++) {
                    if (escape_state == state_esc) {
                        if (buffer[i] == '[') {
                            // ESC [ = CSI
                            escape_state = state_csi;
                        } else if (buffer[i] == ']') {
                            // ESC ] = OSC
                            escape_state = state_osc;
                        } else if (buffer[i] == '=') {
                            // ESC =, enter alternate keypad mode
                            // TODO
                            escape_state = state_idle;
                        } else if (buffer[i] == '>') {
                            // ESC >, exit alternate keypad mode
                            // TODO
                            escape_state = state_idle;
                        } else if (buffer[i] == 'P') {
                            // ESC P = DCS
                            // TODO
                            escape_state = state_dcs;
                        } else {
                            // unknown
                            OH_LOG_WARN(LOG_APP, "Unknown escape sequence after ESC: %{public}s %{public}c",
                                        escape_buffer.c_str(), buffer[i]);
                            escape_state = state_idle;
                        }
                    } else if (escape_state == state_csi) {
                        if (buffer[i] == 'A') {
                            // CSI Ps A, CUU, move cursor up # lines
                            row -= read_int_or_default(1);
                            clamp_row();
                            escape_state = state_idle;
                        } else if (buffer[i] == 'B') {
                            // CSI Ps B, CUD, move cursor down # lines
                            row += read_int_or_default(1);
                            clamp_row();
                            escape_state = state_idle;
                        } else if (buffer[i] == 'C') {
                            // CSI Ps C, CUF, move cursor right # columns
                            col += read_int_or_default(1);
                            clamp_col();
                            escape_state = state_idle;
                        } else if (buffer[i] == 'D') {
                            // CSI Ps D, CUB, move cursor left # columns
                            col -= read_int_or_default(1);
                            clamp_col();
                            escape_state = state_idle;
                        } else if (buffer[i] == 'E') {
                            // CSI Ps E, CNL, move cursor to the beginning of next line, down # lines
                            row += read_int_or_default(1);
                            clamp_row();
                            col = 0;
                            escape_state = state_idle;
                        } else if (buffer[i] == 'F') {
                            // CSI Ps F, CPL, move cursor to the beginning of previous line, up # lines
                            row -= read_int_or_default(1);
                            clamp_row();
                            col = 0;
                            escape_state = state_idle;
                        } else if (buffer[i] == 'G') {
                            // CSI Ps G, CHA, move cursor to column #
                            col = read_int_or_default(1);
                            // convert from 1-based to 0-based
                            col--;
                            clamp_col();
                            escape_state = state_idle;
                        } else if (buffer[i] == 'H') {
                            // CSI Ps ; PS H, CUP, move cursor to x, y, default to upper left corner
                            std::vector<std::string> parts = splitString(escape_buffer, ";");
                            if (parts.size() == 2) {
                                sscanf(parts[0].c_str(), "%d", &row);
                                sscanf(parts[1].c_str(), "%d", &col);
                                // convert from 1-based to 0-based
                                row--;
                                col--;
                                clamp_row();
                                clamp_col();
                            } else if (escape_buffer == "") {
                                row = col = 0;
                            }
                            escape_state = state_idle;
                        } else if (buffer[i] == 'J') {
                            // CSI Ps J, ED, erase in display
                            if (escape_buffer == "" || escape_buffer == "0") {
                                // erase below
                                for (int i = col; i < term_col; i++) {
                                    terminal[row][i] = term_char();
                                }
                                for (int i = row + 1; i < term_row; i++) {
                                    std::fill(terminal[i].begin(), terminal[i].end(), term_char());
                                }
                            } else if (escape_buffer == "1") {
                                // erase above
                                for (int i = 0; i < row; i++) {
                                    std::fill(terminal[i].begin(), terminal[i].end(), term_char());
                                }
                                for (int i = 0; i <= col; i++) {
                                    terminal[row][i] = term_char();
                                }
                            } else if (escape_buffer == "2") {
                                // erase all
                                for (int i = 0; i < term_row; i++) {
                                    std::fill(terminal[i].begin(), terminal[i].end(), term_char());
                                }
                            }
                            escape_state = state_idle;
                        } else if (buffer[i] == 'K') {
                            // CSI Ps K, EL, erase in line
                            if (escape_buffer == "" || escape_buffer == "0") {
                                // erase to right
                                for (int i = col; i < term_col; i++) {
                                    terminal[row][i] = term_char();
                                }
                            } else if (escape_buffer == "1") {
                                // erase to left
                                for (int i = 0; i <= col; i++) {
                                    terminal[row][i] = term_char();
                                }
                            }
                            escape_state = state_idle;
                        } else if (buffer[i] == 'P') {
                            // CSI Ps P, DCH, delete # characters, move right to left
                            int del = read_int_or_default(1);
                            for (int i = col; i < term_col; i++) {
                                if (i + del < term_col) {
                                    terminal[row][i] = terminal[row][i + del];
                                } else {
                                    terminal[row][i] = term_char();
                                }
                            }
                            escape_state = state_idle;
                        } else if (buffer[i] == 'X') {
                            // CSI Ps X, ECH, erase # characters, do not move others
                            int del = read_int_or_default(1);
                            for (int i = col; i < col + del && i < term_col; i++) {
                                terminal[row][i] = term_char();
                            }
                            escape_state = state_idle;
                        } else if (buffer[i] == 'c' && escape_buffer == "") {
                            // CSI Ps c, Send Device Attributes
                            // send CSI ? 6 c: I am VT102
                            uint8_t send_buffer[] = {0x1b, '[', '?', '6', 'c'};
                            int res = write(fd, send_buffer, sizeof(send_buffer));
                            assert(res == sizeof(send_buffer));
                            escape_state = state_idle;
                        } else if (buffer[i] == 'd' && escape_buffer != "") {
                            // CSI Ps d, VPA, move cursor to row #
                            sscanf(escape_buffer.c_str(), "%d", &row);
                            // convert from 1-based to 0-based
                            row--;
                            clamp_row();
                            escape_state = state_idle;
                        } else if (buffer[i] == 'h' && escape_buffer.size() > 0 && escape_buffer[0] == '?') {
                            // CSI ? Pm h, DEC Private Mode Set (DECSET)
                            std::vector<std::string> parts = splitString(escape_buffer.substr(1), ";");
                            for (auto part : parts) {
                                if (part == "1") {
                                    // CSI ? 1 h, Application Cursor Keys (DECCKM)
                                    // TODO
                                } else if (part == "12") {
                                    // CSI ? 12 h, Start blinking cursor
                                    // TODO
                                } else if (part == "25") {
                                    // CSI ? 25 h, DECTCEM, make cursor visible
                                    show_cursor = true;
                                } else if (part == "1000") {
                                    // CSI ? 1000 h, Send Mouse X & Y on button press and release
                                    // TODO
                                } else if (part == "1002") {
                                    // CSI ? 1002 h, Use Cell Motion Mouse Tracking
                                    // TODO
                                } else if (part == "1006") {
                                    // CSI ? 1006 h, Enable SGR Mouse Mode
                                    // TODO
                                } else if (part == "2004") {
                                    // CSI ? 2004 h, set bracketed paste mode
                                    // TODO
                                } else {
                                    OH_LOG_WARN(LOG_APP, "Unknown CSI ? Pm h: %{public}s %{public}c",
                                                escape_buffer.c_str(), buffer[i]);
                                }
                            }
                            escape_state = state_idle;
                        } else if (buffer[i] == 'l' && escape_buffer.size() > 0 && escape_buffer[0] == '?') {
                            // CSI ? Pm l, DEC Private Mode Reset (DECRST)
                            std::vector<std::string> parts = splitString(escape_buffer.substr(1), ";");
                            for (auto part : parts) {
                                if (part == "12") {
                                    // CSI ? 12 l, Stop blinking cursor
                                    // TODO
                                } else if (part == "25") {
                                    // CSI ? 25 l, Hide cursor (DECTCEM)
                                    show_cursor = true;
                                } else if (part == "2004") {
                                    // CSI ? 2004 l, reset bracketed paste mode
                                    // TODO
                                } else {
                                    OH_LOG_WARN(LOG_APP, "Unknown CSI ? Pm l: %{public}s %{public}c",
                                                escape_buffer.c_str(), buffer[i]);
                                }
                            }
                            escape_state = state_idle;
                        } else if (buffer[i] == 'm' && escape_buffer == "") {
                            // CSI Pm m, Character Attributes (SGR)
                            // reset all attributes to their defaults
                            current_style = style();
                            escape_state = state_idle;
                        } else if (buffer[i] == 'm' && escape_buffer.size() > 0 && escape_buffer[0] != '>') {
                            // CSI Pm m, Character Attributes (SGR)

                            // set color
                            std::vector<std::string> parts = splitString(escape_buffer, ";");
                            for (auto part : parts) {
                                if (part == "0") {
                                    // reset all attributes to their defaults
                                    current_style = style();
                                } else if (part == "1" || part == "01") {
                                    // set bold
                                    current_style.weight = weight::bold;
                                } else if (part == "7") {
                                    // inverse
                                    std::swap(current_style.fg_red, current_style.bg_red);
                                    std::swap(current_style.fg_green, current_style.bg_green);
                                    std::swap(current_style.fg_blue, current_style.bg_blue);
                                } else if (part == "10") {
                                    // reset to primary font
                                    current_style = style();
                                } else if (part == "30") {
                                    // black foreground
                                    current_style.fg_red = 0.0;
                                    current_style.fg_green = 0.0;
                                    current_style.fg_blue = 0.0;
                                } else if (part == "31") {
                                    // red foreground
                                    current_style.fg_red = 1.0;
                                    current_style.fg_green = 0.0;
                                    current_style.fg_blue = 0.0;
                                } else if (part == "32") {
                                    // green foreground
                                    current_style.fg_red = 0.0;
                                    current_style.fg_green = 1.0;
                                    current_style.fg_blue = 0.0;
                                } else if (part == "33") {
                                    // yellow foreground
                                    current_style.fg_red = 1.0;
                                    current_style.fg_green = 1.0;
                                    current_style.fg_blue = 0.0;
                                } else if (part == "34") {
                                    // blue foreground
                                    current_style.fg_red = 0.0;
                                    current_style.fg_green = 0.0;
                                    current_style.fg_blue = 1.0;
                                } else if (part == "35") {
                                    // magenta foreground
                                    current_style.fg_red = 1.0;
                                    current_style.fg_green = 0.0;
                                    current_style.fg_blue = 1.0;
                                } else if (part == "36") {
                                    // cyan foreground
                                    current_style.fg_red = 0.0;
                                    current_style.fg_green = 1.0;
                                    current_style.fg_blue = 1.0;
                                } else if (part == "37") {
                                    // white foreground
                                    current_style.fg_red = 1.0;
                                    current_style.fg_green = 1.0;
                                    current_style.fg_blue = 1.0;
                                } else if (part == "39") {
                                    // default foreground
                                    current_style.fg_red = 0.0;
                                    current_style.fg_green = 0.0;
                                    current_style.fg_blue = 0.0;
                                } else if (part == "40") {
                                    // black background
                                    current_style.bg_red = 0.0;
                                    current_style.bg_green = 0.0;
                                    current_style.bg_blue = 0.0;
                                } else if (part == "41") {
                                    // black background
                                    current_style.bg_red = 1.0;
                                    current_style.bg_green = 0.0;
                                    current_style.bg_blue = 0.0;
                                } else if (part == "42") {
                                    // green background
                                    current_style.bg_red = 0.0;
                                    current_style.bg_green = 1.0;
                                    current_style.bg_blue = 0.0;
                                } else if (part == "43") {
                                    // yellow background
                                    current_style.bg_red = 1.0;
                                    current_style.bg_green = 1.0;
                                    current_style.bg_blue = 0.0;
                                } else if (part == "44") {
                                    // blue background
                                    current_style.bg_red = 0.0;
                                    current_style.bg_green = 0.0;
                                    current_style.bg_blue = 1.0;
                                } else if (part == "45") {
                                    // magenta background
                                    current_style.bg_red = 1.0;
                                    current_style.bg_green = 0.0;
                                    current_style.bg_blue = 1.0;
                                } else if (part == "46") {
                                    // cyan background
                                    current_style.bg_red = 0.0;
                                    current_style.bg_green = 1.0;
                                    current_style.bg_blue = 1.0;
                                } else if (part == "47") {
                                    // white background
                                    current_style.bg_red = 1.0;
                                    current_style.bg_green = 1.0;
                                    current_style.bg_blue = 1.0;
                                } else if (part == "49") {
                                    // default background
                                    current_style.bg_red = 1.0;
                                    current_style.bg_green = 1.0;
                                    current_style.bg_blue = 1.0;
                                } else if (part == "90") {
                                    // bright black foreground
                                    current_style.fg_red = 0.5;
                                    current_style.fg_green = 0.5;
                                    current_style.fg_blue = 0.5;
                                } else {
                                    OH_LOG_WARN(LOG_APP, "Unknown CSI Pm m: %{public}s %{public}c",
                                                escape_buffer.c_str(), buffer[i]);
                                }
                            }
                            escape_state = state_idle;
                        } else if (buffer[i] == 'm' && escape_buffer.size() > 0 && escape_buffer[0] == '>') {
                            // CSI > Pp m, XTMODKEYS, set/reset key modifier options
                            // TODO
                            escape_state = state_idle;
                        } else if (buffer[i] == 'n' && escape_buffer == "6") {
                            // CSI Ps n, DSR, Device Status Report
                            // Ps = 6: Report Cursor Position (CPR)
                            // send ESC [ row ; col R
                            char send_buffer[128] = {};
                            snprintf(send_buffer, sizeof(send_buffer), "\x1b[%d;%dR", row + 1, col + 1);
                            int len = strlen(send_buffer);
                            int res = write(fd, send_buffer, len);
                            assert(res == len);
                            escape_state = state_idle;
                        } else if (buffer[i] == '@' &&
                                   ((escape_buffer.size() > 0 && escape_buffer[escape_buffer.size() - 1] >= '0' &&
                                     escape_buffer[escape_buffer.size() - 1] <= '9') ||
                                    escape_buffer == "")) {
                            // CSI Ps @, ICH, Insert Ps (Blank) Character(s)
                            int count = read_int_or_default(1);
                            for (int i = term_col - 1; i >= col; i--) {
                                if (i - col < count) {
                                    terminal[row][col].ch = ' ';
                                } else {
                                    terminal[row][col] = terminal[row][col - count];
                                }
                            }
                            escape_state = state_idle;
                        } else if (buffer[i] == '?' || buffer[i] == ';' || buffer[i] == '>' || buffer[i] == '=' ||
                                   (buffer[i] >= '0' && buffer[i] <= '9')) {
                            // '?', ';', '>', '=' or number
                            escape_buffer += buffer[i];
                        } else {
                            // unknown
                            OH_LOG_WARN(LOG_APP, "Unknown escape sequence in CSI: %{public}s %{public}c",
                                        escape_buffer.c_str(), buffer[i]);
                            escape_state = state_idle;
                        }
                    } else if (escape_state == state_osc) {
                        if (buffer[i] == '\x07') {
                            // OSC Ps ; Pt BEL, do nothing
                            escape_state = state_idle;
                        } else if (i + 1 < r && buffer[i] == '\x1b' && buffer[i] == '\\') {
                            // ST is ESC \
                            // OSC Ps ; Pt ST, TODO
                            i += 1;
                            escape_state = state_idle;
                        } else if (buffer[i] >= ' ' && buffer[i] < 127) {
                            // printable character
                            escape_buffer += buffer[i];
                        } else {
                            // unknown
                            OH_LOG_WARN(LOG_APP, "Unknown escape sequence in OSC: %{public}s %{public}c",
                                        escape_buffer.c_str(), buffer[i]);
                            escape_state = state_idle;
                        }
                    } else if (escape_state == state_dcs) {
                        if (i + 1 < r && buffer[i] == '\x1b' && buffer[i] == '\\') {
                            // ST is ESC \
                            i += 1;
                            escape_state = state_idle;
                        } else if (buffer[i] >= ' ' && buffer[i] < 127) {
                            // printable character
                            escape_buffer += buffer[i];
                        } else {
                            // unknown
                            OH_LOG_WARN(LOG_APP, "Unknown escape sequence in DCS: %{public}s %{public}c",
                                        escape_buffer.c_str(), buffer[i]);
                            escape_state = state_idle;
                        }
                    } else if (escape_state == state_idle) {
                        // escape state is idle
                        if (utf8_state == state_initial) {
                            if (buffer[i] >= ' ' && buffer[i] <= 0x7f) {
                                // printable
                                InsertUtf8(buffer[i]);
                            } else if (buffer[i] >= 0xc2 && buffer[i] <= 0xdf) {
                                // 2-byte utf8
                                utf8_state = state_2byte_2;
                                current_utf8 = (uint32_t)(buffer[i] & 0x1f) << 6;
                            } else if (buffer[i] == 0xe0) {
                                // 3-byte utf8 starting with e0
                                utf8_state = state_3byte_2_e0;
                                current_utf8 = (uint32_t)(buffer[i] & 0x0f) << 12;
                            } else if (buffer[i] >= 0xe1 && buffer[i] <= 0xef) {
                                // 3-byte utf8 starting with non-e0
                                utf8_state = state_3byte_2_non_e0;
                                current_utf8 = (uint32_t)(buffer[i] & 0x0f) << 12;
                            } else if (buffer[i] == 0xf0) {
                                // 4-byte utf8 starting with f0
                                utf8_state = state_4byte_2_f0;
                                current_utf8 = (uint32_t)(buffer[i] & 0x07) << 18;
                            } else if (buffer[i] >= 0xf1 && buffer[i] <= 0xf3) {
                                // 4-byte utf8 starting with f1 to f3
                                utf8_state = state_4byte_2_f1_f3;
                                current_utf8 = (uint32_t)(buffer[i] & 0x07) << 18;
                            } else if (buffer[i] == 0xf4) {
                                // 4-byte utf8 starting with f4
                                utf8_state = state_4byte_2_f4;
                                current_utf8 = (uint32_t)(buffer[i] & 0x07) << 18;
                            } else if (buffer[i] == '\r') {
                                col = 0;
                            } else if (buffer[i] == '\n') {
                                row += 1;
                                DropFirstRowIfOverflow();
                            } else if (buffer[i] == '\b') {
                                if (col > 0) {
                                    col -= 1;
                                }
                            } else if (buffer[i] == '\t') {
                                col = (col + 8) / 8 * 8;
                                if (col >= term_col) {
                                    col = 0;
                                    row++;
                                    DropFirstRowIfOverflow();
                                }
                            } else if (buffer[i] == 0x1b) {
                                escape_buffer = "";
                                escape_state = state_esc;
                            }
                        } else if (utf8_state == state_2byte_2) {
                            // expecting the second byte of 2-byte utf-8
                            if (buffer[i] >= 0x80 && buffer[i] <= 0xbf) {
                                current_utf8 |= (buffer[i] & 0x3f);
                                InsertUtf8(current_utf8);
                            }
                            utf8_state = state_initial;
                        } else if (utf8_state == state_3byte_2_e0) {
                            // expecting the second byte of 3-byte utf-8 starting with 0xe0
                            if (buffer[i] >= 0xa0 && buffer[i] <= 0xbf) {
                                current_utf8 |= (uint32_t)(buffer[i] & 0x3f) << 6;
                                utf8_state = state_3byte_3;
                            } else {
                                utf8_state = state_initial;
                            }
                        } else if (utf8_state == state_3byte_2_non_e0) {
                            // expecting the second byte of 3-byte utf-8 starting with non-0xe0
                            if (buffer[i] >= 0x80 && buffer[i] <= 0xbf) {
                                current_utf8 |= (uint32_t)(buffer[i] & 0x3f) << 6;
                                utf8_state = state_3byte_3;
                            } else {
                                utf8_state = state_initial;
                            }
                        } else if (utf8_state == state_3byte_3) {
                            // expecting the third byte of 3-byte utf-8 starting with 0xe0
                            if (buffer[i] >= 0x80 && buffer[i] <= 0xbf) {
                                current_utf8 |= (buffer[i] & 0x3f);
                                InsertUtf8(current_utf8);
                            }
                            utf8_state = state_initial;
                        } else if (utf8_state == state_4byte_2_f0) {
                            // expecting the second byte of 4-byte utf-8 starting with 0xf0
                            if (buffer[i] >= 0x90 && buffer[i] <= 0xbf) {
                                current_utf8 |= (uint32_t)(buffer[i] & 0x3f) << 12;
                                utf8_state = state_4byte_3;
                            } else {
                                utf8_state = state_initial;
                            }
                        } else if (utf8_state == state_4byte_2_f1_f3) {
                            // expecting the second byte of 4-byte utf-8 starting with 0xf0 to 0xf3
                            if (buffer[i] >= 0x80 && buffer[i] <= 0xbf) {
                                current_utf8 |= (uint32_t)(buffer[i] & 0x3f) << 12;
                                utf8_state = state_4byte_3;
                            } else {
                                utf8_state = state_initial;
                            }
                        } else if (utf8_state == state_4byte_2_f4) {
                            // expecting the second byte of 4-byte utf-8 starting with 0xf4
                            if (buffer[i] >= 0x80 && buffer[i] <= 0x8f) {
                                current_utf8 |= (uint32_t)(buffer[i] & 0x3f) << 12;
                                utf8_state = state_4byte_3;
                            } else {
                                utf8_state = state_initial;
                            }
                        } else if (utf8_state == state_4byte_3) {
                            // expecting the third byte of 4-byte utf-8
                            if (buffer[i] >= 0x80 && buffer[i] <= 0xbf) {
                                current_utf8 |= (uint32_t)(buffer[i] & 0x3f) << 6;
                                utf8_state = state_4byte_4;
                            } else {
                                utf8_state = state_initial;
                            }
                        } else if (utf8_state == state_4byte_4) {
                            // expecting the third byte of 4-byte utf-8
                            if (buffer[i] >= 0x80 && buffer[i] <= 0xbf) {
                                current_utf8 |= (buffer[i] & 0x3f);
                                InsertUtf8(current_utf8);
                            }
                            utf8_state = state_initial;
                        } else {
                            assert(false && "unreachable utf8 state");
                        }
                    } else {
                        assert(false && "unreachable escape state");
                    }
                }
                pthread_mutex_unlock(&lock);
            }
        }
    }
}

static napi_value CreateSurface(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    int64_t surface_id = 0;
    bool lossless = true;
    napi_status res = napi_get_value_bigint_int64(env, args[0], &surface_id, &lossless);
    assert(res == napi_ok);

    // create windows and display
    OHNativeWindow *native_window;
    OH_NativeWindow_CreateNativeWindowFromSurfaceId(surface_id, &native_window);
    assert(native_window);
    EGLNativeWindowType egl_window = (EGLNativeWindowType)native_window;
    egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    assert(egl_display != EGL_NO_DISPLAY);

    // initialize egl
    EGLint major_version;
    EGLint minor_version;
    EGLBoolean egl_res = eglInitialize(egl_display, &major_version, &minor_version);
    assert(egl_res == EGL_TRUE);

    const EGLint attrib[] = {EGL_SURFACE_TYPE,
                             EGL_WINDOW_BIT,
                             EGL_RENDERABLE_TYPE,
                             EGL_OPENGL_ES2_BIT,
                             EGL_RED_SIZE,
                             8,
                             EGL_GREEN_SIZE,
                             8,
                             EGL_BLUE_SIZE,
                             8,
                             EGL_ALPHA_SIZE,
                             8,
                             EGL_DEPTH_SIZE,
                             24,
                             EGL_STENCIL_SIZE,
                             8,
                             EGL_SAMPLE_BUFFERS,
                             1,
                             EGL_SAMPLES,
                             4, // Request 4 samples for multisampling
                             EGL_NONE};

    const EGLint max_config_size = 1;
    EGLint num_configs;
    EGLConfig egl_config;
    egl_res = eglChooseConfig(egl_display, attrib, &egl_config, max_config_size, &num_configs);
    assert(egl_res == EGL_TRUE);

    egl_surface = eglCreateWindowSurface(egl_display, egl_config, egl_window, NULL);

    EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, context_attributes);

    pthread_t render_thread;
    pthread_create(&render_thread, NULL, RenderWorker, NULL);

    pthread_t terminal_thread;
    pthread_create(&terminal_thread, NULL, TerminalWorker, NULL);
    return nullptr;
}

static napi_value ResizeSurface(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    napi_get_value_int32(env, args[1], &width);
    napi_get_value_int32(env, args[2], &height);

    pthread_mutex_lock(&lock);
    term_col = width / font_width;
    term_row = height / font_height;
    terminal.resize(term_row);
    for (int i = 0; i < term_row; i++) {
        terminal[i].resize(term_col);
    }

    if (row > term_row - 1) {
        row = term_row - 1;
    }

    if (col > term_col - 1) {
        col = term_col - 1;
    }
    pthread_mutex_unlock(&lock);

    struct winsize ws = {};
    ws.ws_col = term_col;
    ws.ws_row = term_row;
    ioctl(fd, TIOCSWINSZ, &ws);

    return nullptr;
}

static napi_value Scroll(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    double offset = 0;
    napi_status res = napi_get_value_double(env, args[0], &offset);
    assert(res == napi_ok);

    // natural scrolling
    scroll_offset -= offset;
    if (scroll_offset < 0) {
        scroll_offset = 0.0;
    }

    return nullptr;
}

static napi_value DestroySurface(napi_env env, napi_callback_info info) { return nullptr; }

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"run", nullptr, Run, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"send", nullptr, Send, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"createSurface", nullptr, CreateSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroySurface", nullptr, DestroySurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resizeSurface", nullptr, ResizeSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"scroll", nullptr, Scroll, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module demoModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "entry",
    .nm_priv = ((void *)0),
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterEntryModule(void) { napi_module_register(&demoModule); }
