#include <algorithm>
#include "emu/tracer.h"
#include "hw/holly/ta.h"
#include "hw/holly/trace.h"
#include "ui/window.h"

using namespace re;
using namespace re::emu;
using namespace re::hw::holly;

static const char *s_param_names[] = {
    "TA_PARAM_END_OF_LIST", "TA_PARAM_USER_TILE_CLIP", "TA_PARAM_OBJ_LIST_SET",
    "TA_PARAM_RESERVED0",   "TA_PARAM_POLY_OR_VOL",    "TA_PARAM_SPRITE",
    "TA_PARAM_RESERVED1",   "TA_PARAM_VERTEX",
};

static const char *s_list_names[] = {
    "TA_LIST_OPAQUE",        "TA_LIST_OPAQUE_MODVOL",
    "TA_LIST_TRANSLUCENT",   "TA_LIST_TRANSLUCENT_MODVOL",
    "TA_LIST_PUNCH_THROUGH",
};

static const char *s_pixel_format_names[] = {
    "PXL_INVALID", "PXL_RGBA",     "PXL_RGBA5551",
    "PXL_RGB565",  "PXL_RGBA4444", "PXL_RGBA8888",
};

static const char *s_filter_mode_names[] = {
    "FILTER_NEAREST", "FILTER_BILINEAR",
};

static const char *s_wrap_mode_names[] = {
    "WRAP_REPEAT", "WRAP_CLAMP_TO_EDGE", "WRAP_MIRRORED_REPEAT",
};

static const char *s_depthfunc_names[] = {
    "NONE",    "NEVER",  "LESS",   "EQUAL",  "LEQUAL",
    "GREATER", "NEQUAL", "GEQUAL", "ALWAYS",
};

static const char *s_cullface_names[] = {
    "NONE", "FRONT", "BACK",
};

static const char *s_blendfunc_names[] = {
    "NONE",
    "ZERO",
    "ONE",
    "SRC_COLOR",
    "ONE_MINUS_SRC_COLOR",
    "SRC_ALPHA",
    "ONE_MINUS_SRC_ALPHA",
    "DST_ALPHA",
    "ONE_MINUS_DST_ALPHA",
    "DST_COLOR",
    "ONE_MINUS_DST_COLOR",
};

static const char *s_shademode_names[] = {
    "DECAL", "MODULATE", "DECAL_ALPHA", "MODULATE_ALPHA",
};

static const int INVALID_OFFSET = -1;

void TraceTextureCache::AddTexture(tsp_t tsp, tcw_t &tcw,
                                   const uint8_t *palette,
                                   const uint8_t *texture) {
  texture_key_t texture_key = tr_get_texture_key(tsp, tcw);
  TextureInst &texture_inst = textures_[texture_key];
  texture_inst.tsp = tsp;
  texture_inst.tcw = tcw;
  texture_inst.palette = palette;
  texture_inst.texture = texture;
  texture_inst.handle = 0;
}

void TraceTextureCache::RemoveTexture(tsp_t tsp, tcw_t &tcw) {
  texture_key_t texture_key = tr_get_texture_key(tsp, tcw);
  textures_.erase(texture_key);
}

texture_handle_t TraceTextureCache::GetTexture(
    const ta_ctx_t &tctx, tsp_t tsp, tcw_t tcw,
    register_texture_cb register_cb) {
  texture_key_t texture_key = tr_get_texture_key(tsp, tcw);

  auto it = textures_.find(texture_key);
  CHECK_NE(it, textures_.end(), "Texture wasn't available in cache");

  TextureInst &texture = it->second;

  // register the texture if it hasn't already been
  if (!texture.handle) {
    registered_texture_t res =
        register_delegate(tctx, tsp, tcw, texture.palette, texture.texture);
    texture.handle = res.handle;
    texture.format = res.format;
    texture.filter = res.filter;
    texture.wrap_u = res.wrap_u;
    texture.wrap_v = res.wrap_v;
    texture.mipmaps = res.mipmaps;
    texture.width = res.width;
    texture.height = res.height;
  }

  return texture.handle;
}

Tracer::Tracer(window_t &window)
    : window_(window),
      rb_(window.render_backend()),
      tile_renderer_(rb_, texcache_),
      hide_params_() {
  window_.AddListener(this);
}

Tracer::~Tracer() {
  window_.RemoveListener(this);
}

void Tracer::Run(const char *path) {
  if (!Parse(path)) {
    return;
  }

  running_ = true;

  while (running_) {
    window_.PumpEvents();
  }
}

void Tracer::OnPaint(bool show_main_menu) {
  tile_renderer_.ParseContext(tctx_, &rctx_, true);

  // render UI
  RenderScrubberMenu();
  RenderTextureMenu();
  RenderContextMenu();

  // clamp surfaces the last surface belonging to the current param
  int n = rctx_.surfs.size();
  int last_idx = n;

  if (current_offset_ != INVALID_OFFSET) {
    const auto &param_entry = rctx_.param_map[current_offset_];
    last_idx = param_entry.num_surfs;
  }

  // render the context
  rb_->BeginSurfaces(rctx_.projection, rctx_.verts.data(), rctx_.verts.size());

  for (int i = 0; i < n; i++) {
    int idx = rctx_.sorted_surfs[i];

    // if this surface comes after the current parameter, ignore it
    if (idx >= last_idx) {
      continue;
    }

    rb_->DrawSurface(rctx_.surfs[idx]);
  }

  rb_->EndSurfaces();
}

void Tracer::OnKeyDown(keycode_t code, int16_t value) {
  if (code == K_F1) {
    if (value) {
      window_.EnableMainMenu(!window_.MainMenuEnabled());
    }
  } else if (code == K_LEFT && value) {
    PrevContext();
  } else if (code == K_RIGHT && value) {
    NextContext();
  } else if (code == K_UP && value) {
    PrevParam();
  } else if (code == K_DOWN && value) {
    NextParam();
  }
}

void Tracer::OnClose() {
  running_ = false;
}

bool Tracer::Parse(const char *path) {
  if (!trace_.Parse(path)) {
    LOG_WARNING("Failed to parse %s", path);
    return false;
  }

  ResetContext();

  return true;
}

void Tracer::RenderScrubberMenu() {
  ImGuiIO &io = ImGui::GetIO();

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::Begin("Scrubber", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

  ImGui::SetWindowSize(ImVec2(io.DisplaySize.x, 0.0f));
  ImGui::SetWindowPos(ImVec2(0.0f, 0.0f));

  ImGui::PushItemWidth(-1.0f);
  int frame = current_frame_;
  if (ImGui::SliderInt("", &frame, 0, num_frames_ - 1)) {
    int delta = frame - current_frame_;
    for (int i = 0; i < std::abs(delta); i++) {
      if (delta > 0) {
        NextContext();
      } else {
        PrevContext();
      }
    }
  }
  ImGui::PopItemWidth();

  ImGui::End();
  ImGui::PopStyleVar();
}

void Tracer::RenderTextureMenu() {
  ImGuiIO &io = ImGui::GetIO();

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::Begin("Textures", nullptr, ImGuiWindowFlags_NoTitleBar |
                                        ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_HorizontalScrollbar);

  ImGui::SetWindowSize(ImVec2(io.DisplaySize.x, 0.0f));
  ImGui::SetWindowPos(
      ImVec2(0.0f, io.DisplaySize.y - ImGui::GetWindowSize().y));

  auto begin = texcache_.textures_begin();
  auto end = texcache_.textures_end();

  for (auto it = begin; it != end; ++it) {
    const TextureInst &tex = it->second;
    ImTextureID handle_id =
        reinterpret_cast<ImTextureID>(static_cast<intptr_t>(tex.handle));

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::ImageButton(handle_id, ImVec2(32.0f, 32.0f), ImVec2(0.0f, 1.0f),
                       ImVec2(1.0f, 0.0f));
    ImGui::PopStyleColor();

    char popup_name[128];
    snprintf(popup_name, sizeof(popup_name), "texture_%d", tex.handle);

    if (ImGui::BeginPopupContextItem(popup_name, 0)) {
      ImGui::Image(handle_id, ImVec2(128, 128), ImVec2(0.0f, 1.0f),
                   ImVec2(1.0f, 0.0f));

      ImGui::Separator();

      ImGui::Text("addr; 0x%08x", tex.tcw.texture_addr << 3);
      ImGui::Text("format: %s", s_pixel_format_names[tex.format]);
      ImGui::Text("filter: %s", s_filter_mode_names[tex.filter]);
      ImGui::Text("wrap_u: %s", s_wrap_mode_names[tex.wrap_u]);
      ImGui::Text("wrap_v: %s", s_wrap_mode_names[tex.wrap_v]);
      ImGui::Text("mipmaps: %d", tex.mipmaps);
      ImGui::Text("width: %d", tex.width);
      ImGui::Text("height: %d", tex.height);

      ImGui::EndPopup();
    }

    ImGui::SameLine();
  }

  ImGui::End();
  ImGui::PopStyleVar();
}

void Tracer::FormatTooltip(int list_type, int vertex_type, int offset) {
  int surf_id = rctx_.param_map[offset].num_surfs - 1;
  int vert_id = rctx_.param_map[offset].num_verts - 1;

  ImGui::BeginTooltip();

  ImGui::Text("list type: %s", s_list_names[list_type]);
  ImGui::Text("surf: %d", surf_id);

  {
    // find sorted position
    int sort = 0;
    for (int i = 0, n = rctx_.surfs.size(); i < n; i++) {
      int idx = rctx_.sorted_surfs[i];
      if (idx == surf_id) {
        sort = i;
        break;
      }
    }
    ImGui::Text("sort: %d", sort);
  }

  // render source TA information
  if (vertex_type == -1) {
    const poly_param_t *param =
        reinterpret_cast<const poly_param_t *>(tctx_.data + offset);

    ImGui::Text("pcw: 0x%x", param->type0.pcw.full);
    ImGui::Text("isp_tsp: 0x%x", param->type0.isp_tsp.full);
    ImGui::Text("tsp: 0x%x", param->type0.tsp.full);
    ImGui::Text("tcw: 0x%x", param->type0.tcw.full);

    int poly_type = ta_get_poly_type(param->type0.pcw);

    switch (poly_type) {
      case 1:
        ImGui::Text("face_color_a: %.2f", param->type1.face_color_a);
        ImGui::Text("face_color_r: %.2f", param->type1.face_color_r);
        ImGui::Text("face_color_g: %.2f", param->type1.face_color_g);
        ImGui::Text("face_color_b: %.2f", param->type1.face_color_b);
        break;

      case 2:
        ImGui::Text("face_color_a: %.2f", param->type2.face_color_a);
        ImGui::Text("face_color_r: %.2f", param->type2.face_color_r);
        ImGui::Text("face_color_g: %.2f", param->type2.face_color_g);
        ImGui::Text("face_color_b: %.2f", param->type2.face_color_b);
        ImGui::Text("face_offset_color_a: %.2f",
                    param->type2.face_offset_color_a);
        ImGui::Text("face_offset_color_r: %.2f",
                    param->type2.face_offset_color_r);
        ImGui::Text("face_offset_color_g: %.2f",
                    param->type2.face_offset_color_g);
        ImGui::Text("face_offset_color_b: %.2f",
                    param->type2.face_offset_color_b);
        break;

      case 5:
        ImGui::Text("base_color: 0x%x", param->sprite.base_color);
        ImGui::Text("offset_color: 0x%x", param->sprite.offset_color);
        break;
    }
  } else {
    const vert_param_t *param =
        reinterpret_cast<const vert_param_t *>(tctx_.data + offset);

    ImGui::Text("vert type: %d", vertex_type);

    switch (vertex_type) {
      case 0:
        ImGui::Text("xyz: {%.2f, %.2f, %.2f}", param->type0.xyz[0],
                    param->type0.xyz[1], param->type0.xyz[2]);
        ImGui::Text("base_color: 0x%x", param->type0.base_color);
        break;

      case 1:
        ImGui::Text("xyz: {%.2f, %.2f, %.2f}", param->type1.xyz[0],
                    param->type1.xyz[1], param->type1.xyz[2]);
        ImGui::Text("base_color_a: %.2f", param->type1.base_color_a);
        ImGui::Text("base_color_r: %.2f", param->type1.base_color_r);
        ImGui::Text("base_color_g: %.2f", param->type1.base_color_g);
        ImGui::Text("base_color_b: %.2f", param->type1.base_color_b);
        break;

      case 2:
        ImGui::Text("xyz: {%.2f, %.2f, %.2f}", param->type2.xyz[0],
                    param->type2.xyz[1], param->type2.xyz[2]);
        ImGui::Text("base_intensity: %.2f", param->type2.base_intensity);
        break;

      case 3:
        ImGui::Text("xyz: {%.2f, %.2f, %.2f}", param->type3.xyz[0],
                    param->type3.xyz[1], param->type3.xyz[2]);
        ImGui::Text("uv: {%.2f, %.2f}", param->type3.uv[0], param->type3.uv[1]);
        ImGui::Text("base_color: 0x%x", param->type3.base_color);
        ImGui::Text("offset_color: 0x%x", param->type3.offset_color);
        break;

      case 4:
        ImGui::Text("xyz: {%.2f, %.2f, %.2f}", param->type4.xyz[0],
                    param->type4.xyz[1], param->type4.xyz[2]);
        ImGui::Text("uv: {0x%x, 0x%x}", param->type4.uv[0], param->type4.uv[1]);
        ImGui::Text("base_color: 0x%x", param->type4.base_color);
        ImGui::Text("offset_color: 0x%x", param->type4.offset_color);
        break;

      case 5:
        ImGui::Text("xyz: {%.2f, %.2f, %.2f}", param->type5.xyz[0],
                    param->type5.xyz[1], param->type5.xyz[2]);
        ImGui::Text("uv: {%.2f, %.2f}", param->type5.uv[0], param->type5.uv[1]);
        ImGui::Text("base_color_a: %.2f", param->type5.base_color_a);
        ImGui::Text("base_color_r: %.2f", param->type5.base_color_r);
        ImGui::Text("base_color_g: %.2f", param->type5.base_color_g);
        ImGui::Text("base_color_b: %.2f", param->type5.base_color_b);
        ImGui::Text("offset_color_a: %.2f", param->type5.offset_color_a);
        ImGui::Text("offset_color_r: %.2f", param->type5.offset_color_r);
        ImGui::Text("offset_color_g: %.2f", param->type5.offset_color_g);
        ImGui::Text("offset_color_b: %.2f", param->type5.offset_color_b);
        break;

      case 6:
        ImGui::Text("xyz: {%.2f, %.2f, %.2f}", param->type6.xyz[0],
                    param->type6.xyz[1], param->type6.xyz[2]);
        ImGui::Text("uv: {0x%x, 0x%x}", param->type6.uv[0], param->type6.uv[1]);
        ImGui::Text("base_color_a: %.2f", param->type6.base_color_a);
        ImGui::Text("base_color_r: %.2f", param->type6.base_color_r);
        ImGui::Text("base_color_g: %.2f", param->type6.base_color_g);
        ImGui::Text("base_color_b: %.2f", param->type6.base_color_b);
        ImGui::Text("offset_color_a: %.2f", param->type6.offset_color_a);
        ImGui::Text("offset_color_r: %.2f", param->type6.offset_color_r);
        ImGui::Text("offset_color_g: %.2f", param->type6.offset_color_g);
        ImGui::Text("offset_color_b: %.2f", param->type6.offset_color_b);
        break;

      case 7:
        ImGui::Text("xyz: {%.2f, %.2f, %.2f}", param->type7.xyz[0],
                    param->type7.xyz[1], param->type7.xyz[2]);
        ImGui::Text("uv: {%.2f, %.2f}", param->type7.uv[0], param->type7.uv[1]);
        ImGui::Text("base_intensity: %.2f", param->type7.base_intensity);
        ImGui::Text("offset_intensity: %.2f", param->type7.offset_intensity);
        break;

      case 8:
        ImGui::Text("xyz: {%.2f, %.2f, %.2f}", param->type8.xyz[0],
                    param->type8.xyz[1], param->type8.xyz[2]);
        ImGui::Text("uv: {0x%x, 0x%x}", param->type8.uv[0], param->type8.uv[1]);
        ImGui::Text("base_intensity: %.2f", param->type8.base_intensity);
        ImGui::Text("offset_intensity: %.2f", param->type8.offset_intensity);
        break;
    }
  }

  // always render translated surface information. new surfaces can be created
  // without receiving a new TA_PARAM_POLY_OR_VOL / TA_PARAM_SPRITE
  surface_t &surf = rctx_.surfs[surf_id];

  ImGui::Separator();

  ImGui::Image(
      reinterpret_cast<ImTextureID>(static_cast<intptr_t>(surf.texture)),
      ImVec2(64.0f, 64.0f));
  ImGui::Text("depth_write: %d", surf.depth_write);
  ImGui::Text("depth_func: %s", s_depthfunc_names[surf.depth_func]);
  ImGui::Text("cull: %s", s_cullface_names[surf.cull]);
  ImGui::Text("src_blend: %s", s_blendfunc_names[surf.src_blend]);
  ImGui::Text("dst_blend: %s", s_blendfunc_names[surf.dst_blend]);
  ImGui::Text("shade: %s", s_shademode_names[surf.shade]);
  ImGui::Text("ignore_tex_alpha: %d", surf.ignore_tex_alpha);
  ImGui::Text("first_vert: %d", surf.first_vert);
  ImGui::Text("num_verts: %d", surf.num_verts);

  // render translated vert only when rendering a vertex tooltip
  if (vertex_type != -1) {
    vertex_t &vert = rctx_.verts[vert_id];

    ImGui::Separator();

    ImGui::Text("xyz: {%.2f, %.2f, %.2f}", vert.xyz[0], vert.xyz[1],
                vert.xyz[2]);
    ImGui::Text("uv: {%.2f, %.2f}", vert.uv[0], vert.uv[1]);
    ImGui::Text("color: 0x%08x", vert.color);
    ImGui::Text("offset_color: 0x%08x", vert.offset_color);
  }

  ImGui::EndTooltip();
}

void Tracer::RenderContextMenu() {
  char label[128];

  ImGui::Begin("Context", nullptr, ImVec2(256.0f, 256.0f));

  // param filters
  for (int i = 0; i < TA_NUM_PARAMS; i++) {
    snprintf(label, sizeof(label), "Hide %s", s_param_names[i]);
    ImGui::Checkbox(label, &hide_params_[i]);
  }
  ImGui::Separator();

  // param list
  int list_type = 0;
  int vertex_type = 0;

  for (auto it : rctx_.param_map) {
    int offset = it.first;
    pcw_t pcw = load<pcw_t>(tctx_.data + offset);
    bool param_selected = offset == current_offset_;

    if (!hide_params_[pcw.para_type]) {
      snprintf(label, sizeof(label), "0x%04x %s", offset,
               s_param_names[pcw.para_type]);
      ImGui::Selectable(label, &param_selected);

      switch (pcw.para_type) {
        case TA_PARAM_POLY_OR_VOL:
        case TA_PARAM_SPRITE: {
          const poly_param_t *param =
              reinterpret_cast<const poly_param_t *>(tctx_.data + offset);
          list_type = param->type0.pcw.list_type;
          vertex_type = ta_get_vert_type(param->type0.pcw);

          if (ImGui::IsItemHovered()) {
            FormatTooltip(list_type, -1, offset);
          }
        } break;

        case TA_PARAM_VERTEX: {
          if (ImGui::IsItemHovered()) {
            FormatTooltip(list_type, vertex_type, offset);
          }
        } break;
      }

      if (param_selected) {
        current_offset_ = offset;

        if (scroll_to_param_) {
          if (!ImGui::IsItemVisible()) {
            ImGui::SetScrollHere();
          }

          scroll_to_param_ = false;
        }
      }
    }
  }

  ImGui::End();
}

// Copy TRACE_CMD_CONTEXT command to the current context being rendered.
void Tracer::CopyCommandToContext(const TraceCommand *cmd, ta_ctx_t *ctx) {
  CHECK_EQ(cmd->type, TRACE_CMD_CONTEXT);

  ctx->autosort = cmd->context.autosort;
  ctx->stride = cmd->context.stride;
  ctx->pal_pxl_format = cmd->context.pal_pxl_format;
  ctx->bg_isp = cmd->context.bg_isp;
  ctx->bg_tsp = cmd->context.bg_tsp;
  ctx->bg_tcw = cmd->context.bg_tcw;
  ctx->bg_depth = cmd->context.bg_depth;
  ctx->video_width = cmd->context.video_width;
  ctx->video_height = cmd->context.video_height;
  memcpy(ctx->bg_vertices, cmd->context.bg_vertices,
         cmd->context.bg_vertices_size);
  memcpy(ctx->data, cmd->context.data, cmd->context.data_size);
  ctx->size = cmd->context.data_size;
}

void Tracer::PrevContext() {
  TraceCommand *begin = current_cmd_->prev;
  ;

  // ensure that there is a prev context
  TraceCommand *prev = begin;

  while (prev) {
    if (prev->type == TRACE_CMD_CONTEXT) {
      break;
    }

    prev = prev->prev;
  }

  if (!prev) {
    return;
  }

  // walk back to the prev context, reverting any textures that've been added
  TraceCommand *curr = prev;

  while (curr != prev) {
    if (curr->type == TRACE_CMD_TEXTURE) {
      texcache_.RemoveTexture(curr->texture.tsp, curr->texture.tcw);

      TraceCommand *override = curr->override;
      if (override) {
        CHECK_EQ(override->type, TRACE_CMD_TEXTURE);

        texcache_.AddTexture(override->texture.tsp, override->texture.tcw,
                             override->texture.palette,
                             override->texture.texture);
      }
    }

    curr = curr->prev;
  }

  current_cmd_ = curr;
  current_frame_--;
  CopyCommandToContext(current_cmd_, &tctx_);
  ResetParam();
}

void Tracer::NextContext() {
  TraceCommand *begin = current_cmd_ ? current_cmd_->next : trace_.cmds();

  // ensure that there is a next context
  TraceCommand *next = begin;

  while (next) {
    if (next->type == TRACE_CMD_CONTEXT) {
      break;
    }

    next = next->next;
  }

  if (!next) {
    return;
  }

  // walk towards to the next context, adding any new textures
  TraceCommand *curr = begin;

  while (curr != next) {
    if (curr->type == TRACE_CMD_TEXTURE) {
      texcache_.AddTexture(curr->texture.tsp, curr->texture.tcw,
                           curr->texture.palette, curr->texture.texture);
    }

    curr = curr->next;
  }

  current_cmd_ = curr;
  current_frame_++;
  CopyCommandToContext(current_cmd_, &tctx_);
  ResetParam();
}

void Tracer::ResetContext() {
  // calculate the total number of frames for the trace
  TraceCommand *cmd = trace_.cmds();

  num_frames_ = 0;

  while (cmd) {
    if (cmd->type == TRACE_CMD_CONTEXT) {
      num_frames_++;
    }

    cmd = cmd->next;
  }

  // start rendering the first context
  current_frame_ = -1;
  current_cmd_ = nullptr;
  NextContext();
}

void Tracer::PrevParam() {
  auto it = rctx_.param_map.find(current_offset_);

  if (it == rctx_.param_map.end()) {
    return;
  }

  while (true) {
    // stop at first param
    if (it == rctx_.param_map.begin()) {
      break;
    }

    --it;

    int offset = it->first;
    pcw_t pcw = load<pcw_t>(tctx_.data + offset);

    // found the next visible param
    if (!hide_params_[pcw.para_type]) {
      current_offset_ = it->first;
      scroll_to_param_ = true;
      break;
    }
  }
}

void Tracer::NextParam() {
  auto it = rctx_.param_map.find(current_offset_);

  if (it == rctx_.param_map.end()) {
    return;
  }

  while (true) {
    ++it;

    // stop at last param
    if (it == rctx_.param_map.end()) {
      break;
    }

    int offset = it->first;
    pcw_t pcw = load<pcw_t>(tctx_.data + offset);

    // found the next visible param
    if (!hide_params_[pcw.para_type]) {
      current_offset_ = it->first;
      scroll_to_param_ = true;
      break;
    }
  }
}

void Tracer::ResetParam() {
  current_offset_ = INVALID_OFFSET;
  scroll_to_param_ = false;
}
