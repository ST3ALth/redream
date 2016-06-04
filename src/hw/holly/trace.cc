#include <unordered_map>
#include "core/assert.h"
#include "hw/holly/ta.h"
#include "hw/holly/tr.h"
#include "hw/holly/trace.h"
#include "sys/filesystem.h"

void GetNextTraceFilename(char *filename, size_t size) {
  const char *appdir = fs_appdir();

  for (int i = 0; i < INT_MAX; i++) {
    snprintf(filename, size, "%s" PATH_SEPARATOR "%d.trace", appdir, i);

    if (!fs_exists(filename)) {
      return;
    }
  }

  LOG_FATAL("Unable to find available trace filename");
}

TraceReader::TraceReader() : trace_size_(0), trace_(nullptr) {}

TraceReader::~TraceReader() {
  Reset();
}

bool TraceReader::Parse(const char *filename) {
  Reset();

  FILE *fp = fopen(filename, "rb");
  if (!fp) {
    return false;
  }

  fseek(fp, 0, SEEK_END);
  trace_size_ = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  trace_ = new uint8_t[trace_size_];
  CHECK_EQ(fread(trace_, trace_size_, 1, fp), 1);
  fclose(fp);

  if (!PatchPointers()) {
    return false;
  }

  if (!PatchOverrides()) {
    return false;
  }

  return true;
}

void TraceReader::Reset() {
  if (trace_) {
    delete[] trace_;
  }
}

// Commands are written out with null list pointers, and pointers to data
// are written out relative to the command itself. Set the list pointers,
// and make the data pointers absolute.
bool TraceReader::PatchPointers() {
  TraceCommand *prev_cmd = nullptr;
  TraceCommand *curr_cmd = nullptr;
  uint8_t *ptr = trace_;
  uint8_t *end = trace_ + trace_size_;

  while (ptr < end) {
    prev_cmd = curr_cmd;
    curr_cmd = reinterpret_cast<TraceCommand *>(ptr);

    // set prev / next pointers
    if (prev_cmd) {
      prev_cmd->next = curr_cmd;
    }
    curr_cmd->prev = prev_cmd;
    curr_cmd->next = nullptr;
    curr_cmd->override = nullptr;

    // patch relative data pointers
    switch (curr_cmd->type) {
      case TRACE_CMD_TEXTURE: {
        curr_cmd->texture.palette += reinterpret_cast<intptr_t>(ptr);
        curr_cmd->texture.texture += reinterpret_cast<intptr_t>(ptr);
        ptr += sizeof(*curr_cmd) + curr_cmd->texture.palette_size +
               curr_cmd->texture.texture_size;
      } break;

      case TRACE_CMD_CONTEXT: {
        curr_cmd->context.bg_vertices += reinterpret_cast<intptr_t>(ptr);
        curr_cmd->context.data += reinterpret_cast<intptr_t>(ptr);
        ptr += sizeof(*curr_cmd) + curr_cmd->context.bg_vertices_size +
               curr_cmd->context.data_size;
      } break;

      default:
        LOG_INFO("Unexpected trace command type %d", curr_cmd->type);
        return false;
    }
  }

  return true;
}

// For commands which mutate global state, the previous state needs to be
// tracked in order to support unwinding. To do so, each command is iterated
// and tagged with the previous command that it overrides.
bool TraceReader::PatchOverrides() {
  TraceCommand *cmd = cmds();

  std::unordered_map<texture_key_t, TraceCommand *> last_inserts;

  while (cmd) {
    if (cmd->type == TRACE_CMD_TEXTURE) {
      texture_key_t texture_key =
          tr_get_texture_key(cmd->texture.tsp, cmd->texture.tcw);
      auto last_insert = last_inserts.find(texture_key);

      if (last_insert != last_inserts.end()) {
        cmd->override = last_insert->second;
        last_insert->second = cmd;
      } else {
        last_inserts.insert(std::make_pair(texture_key, cmd));
      }
    }

    cmd = cmd->next;
  }

  return true;
}

TraceWriter::TraceWriter() : file_(nullptr) {}

TraceWriter::~TraceWriter() {
  Close();
}

bool TraceWriter::Open(const char *filename) {
  Close();

  file_ = fopen(filename, "wb");

  return !!file_;
}

void TraceWriter::Close() {
  if (file_) {
    fclose(file_);
  }
}

void TraceWriter::WriteInsertTexture(tsp_t tsp, tcw_t tcw,
                                     const uint8_t *palette, int palette_size,
                                     const uint8_t *texture, int texture_size) {
  TraceCommand cmd;
  cmd.type = TRACE_CMD_TEXTURE;
  cmd.texture.tsp = tsp;
  cmd.texture.tcw = tcw;
  cmd.texture.palette_size = palette_size;
  cmd.texture.palette = reinterpret_cast<const uint8_t *>(sizeof(cmd));
  cmd.texture.texture_size = texture_size;
  cmd.texture.texture =
      reinterpret_cast<const uint8_t *>(sizeof(cmd) + palette_size);

  CHECK_EQ(fwrite(&cmd, sizeof(cmd), 1, file_), 1);
  if (palette_size) {
    CHECK_EQ(fwrite(palette, palette_size, 1, file_), 1);
  }
  if (texture_size) {
    CHECK_EQ(fwrite(texture, texture_size, 1, file_), 1);
  }
}

void TraceWriter::WriteRenderContext(ta_ctx_t *tctx) {
  TraceCommand cmd;
  cmd.type = TRACE_CMD_CONTEXT;
  cmd.context.autosort = tctx->autosort;
  cmd.context.stride = tctx->stride;
  cmd.context.pal_pxl_format = tctx->pal_pxl_format;
  cmd.context.video_width = tctx->video_width;
  cmd.context.video_height = tctx->video_height;
  cmd.context.bg_isp = tctx->bg_isp;
  cmd.context.bg_tsp = tctx->bg_tsp;
  cmd.context.bg_tcw = tctx->bg_tcw;
  cmd.context.bg_depth = tctx->bg_depth;
  cmd.context.bg_vertices_size = sizeof(tctx->bg_vertices);
  cmd.context.bg_vertices = reinterpret_cast<const uint8_t *>(sizeof(cmd));
  cmd.context.data_size = tctx->size;
  cmd.context.data = reinterpret_cast<const uint8_t *>(
      sizeof(cmd) + sizeof(tctx->bg_vertices));

  CHECK_EQ(fwrite(&cmd, sizeof(cmd), 1, file_), 1);
  CHECK_EQ(fwrite(tctx->bg_vertices, sizeof(tctx->bg_vertices), 1, file_), 1);
  if (tctx->size) {
    CHECK_EQ(fwrite(tctx->data, tctx->size, 1, file_), 1);
  }
}
