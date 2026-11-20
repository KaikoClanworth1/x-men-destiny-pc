// X-Men: Destiny - the game's Flash menu movies changed as they load (see menu_patch.h).
//
// What the two movies look like (tools\gfx_dis.py prints all of it):
//
// XMDMenuMain: four buttons MainMenuNav1..4 (sprite 13) on the root timeline at depths 68, 60, 52, 44, faded
// in and out frame by frame by "move" tags; the script of frame 0 makes `mMainListView` (the game's own list
// class, CTextBoxListView) and gives it the four buttons as its slots. The game fills the list
// (_root.AddMainListViewItem) and is told which entry was chosen: OnSelectMenuOption(index).
//   -> a fifth button MainMenuNav5 at depth 36: every tag for depth 44 is copied for it, moved down; the
//      script gets one more AddReference.
//
// XMDMenuOptions: the list is five buttons Settings_Nav_MC1..5 inside the sprite Settings_MC; each sub-menu
// is a frame label (Video, Audio, Controls, Game, Difficulty) with its page sprite at depth 18 and an
// invisible button at depth 122 whose key handlers drive it.
//   -> a sixth button Settings_Nav_MC6, and after the last frame a new one, "Graphics": a page of rows like
//      the Game page's (tick box + text), the same list class, and key handlers that tell the port what was
//      pressed (ExternalInterface calls named PCGraphics...; pc_menu.cpp answers them). The Options list's
//      script also gets the function AddMainListViewItemDisabled, which the game calls and the disc's movie lacks.

#include "menu_patch.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

namespace xmd::menupatch {

namespace {

using Bytes = std::vector<uint8_t>;

uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
void Put16(Bytes& out, uint32_t v) {
  out.push_back(uint8_t(v));
  out.push_back(uint8_t(v >> 8));
}
void Put32(Bytes& out, uint32_t v) {
  Put16(out, v & 0xFFFF);
  Put16(out, v >> 16);
}
void PutText(Bytes& out, const std::string& s) {
  out.insert(out.end(), s.begin(), s.end());
  out.push_back(0);
}
void Append(Bytes& out, const uint8_t* from, size_t count) { out.insert(out.end(), from, from + count); }
void Append(Bytes& out, const Bytes& more) { out.insert(out.end(), more.begin(), more.end()); }

bool Holds(const uint8_t* data, size_t size, const char* text) {
  const size_t n = std::strlen(text);
  for (size_t i = 0; i + n <= size; ++i) {
    if (std::memcmp(data + i, text, n) == 0) return true;
  }
  return false;
}

// ---- tags ----

struct Tag {
  uint16_t code = 0;
  size_t at = 0;    // (its header)
  size_t body = 0;
  size_t size = 0;  // (of the body)
  size_t end() const { return body + size; }
};

bool ReadTag(const uint8_t* data, size_t limit, size_t at, Tag* tag) {
  if (at + 2 > limit) return false;
  const uint16_t head = U16(data + at);
  tag->code = head >> 6;
  tag->at = at;
  tag->size = head & 0x3F;
  tag->body = at + 2;
  if (tag->size == 0x3F) {
    if (at + 6 > limit) return false;
    tag->size = U32(data + at + 2);
    tag->body = at + 6;
  }
  return tag->end() <= limit;
}

void PutTag(Bytes& out, uint16_t code, const Bytes& body) {
  if (body.size() >= 0x3F) {
    Put16(out, uint32_t(code << 6) | 0x3F);
    Put32(out, uint32_t(body.size()));
  } else {
    Put16(out, uint32_t(code << 6) | uint32_t(body.size()));
  }
  Append(out, body);
}

enum : uint16_t {
  kEnd = 0,
  kShowFrame = 1,
  kDoAction = 12,
  kPlaceObject2 = 26,
  kRemoveObject2 = 28,
  kDefineButton2 = 34,
  kDefineEditText = 37,
  kDefineSprite = 39,
  kFrameLabel = 43,
  kCsmTextSettings = 74,
  kExporterInfo = 1000,
};

// ---- bit fields ----

struct BitReader {
  const uint8_t* data;
  size_t at;
  int bit = 0;
  uint32_t U(int count) {
    uint32_t v = 0;
    for (int i = 0; i < count; ++i) {
      v = (v << 1) | ((data[at] >> (7 - bit)) & 1);
      if (++bit == 8) bit = 0, ++at;
    }
    return v;
  }
  int32_t S(int count) {
    if (!count) return 0;
    const uint32_t v = U(count);
    return (v >> (count - 1)) ? int32_t(v) - (int32_t(1) << count) : int32_t(v);
  }
  size_t Align() {
    if (bit) bit = 0, ++at;
    return at;
  }
};

struct BitWriter {
  Bytes bytes;
  int bit = 0;
  void U(uint32_t v, int count) {
    for (int i = count - 1; i >= 0; --i) {
      if (!bit) bytes.push_back(0);
      bytes.back() |= uint8_t(((v >> i) & 1) << (7 - bit));
      bit = (bit + 1) & 7;
    }
  }
  void S(int32_t v, int count) { U(uint32_t(v) & ((count >= 32) ? 0xFFFFFFFFu : ((1u << count) - 1)), count); }
};

int BitsFor(std::initializer_list<int32_t> values) {
  int most = 1;
  for (int32_t v : values) {
    uint32_t m = uint32_t(v < 0 ? ~v : v);
    int bits = 1;
    while (m) ++bits, m >>= 1;
    if (bits > most) most = bits;
  }
  return most;
}

// A placement's matrix: x' = sx*x + r1*y + tx, y' = r0*x + sy*y + ty (16.16 numbers; tx, ty in twips, 20 a
// pixel).
struct Matrix {
  bool scaled = false, rotated = false;
  int32_t sx = 65536, sy = 65536, r0 = 0, r1 = 0, tx = 0, ty = 0;
};
Matrix ReadMatrix(BitReader& in) {
  Matrix m;
  if ((m.scaled = in.U(1))) {
    const int n = int(in.U(5));
    m.sx = in.S(n);
    m.sy = in.S(n);
  }
  if ((m.rotated = in.U(1))) {
    const int n = int(in.U(5));
    m.r0 = in.S(n);
    m.r1 = in.S(n);
  }
  const int n = int(in.U(5));
  m.tx = in.S(n);
  m.ty = in.S(n);
  in.Align();
  return m;
}
Bytes WriteMatrix(const Matrix& m) {
  BitWriter out;
  out.U(m.scaled, 1);
  if (m.scaled) {
    const int n = BitsFor({m.sx, m.sy});
    out.U(uint32_t(n), 5);
    out.S(m.sx, n);
    out.S(m.sy, n);
  }
  out.U(m.rotated, 1);
  if (m.rotated) {
    const int n = BitsFor({m.r0, m.r1});
    out.U(uint32_t(n), 5);
    out.S(m.r0, n);
    out.S(m.r1, n);
  }
  const int n = BitsFor({m.tx, m.ty});
  out.U(uint32_t(n), 5);
  out.S(m.tx, n);
  out.S(m.ty, n);
  return out.bytes;
}
int32_t Fixed(double v) { return int32_t(std::lround(v * 65536.0)); }
int32_t Twips(double pixels) { return int32_t(std::lround(pixels * 20.0)); }
Matrix At(double x, double y, double scale = 1.0) {
  Matrix m;
  m.tx = Twips(x);
  m.ty = Twips(y);
  if (scale != 1.0) m.scaled = true, m.sx = m.sy = Fixed(scale);
  return m;
}

// A PlaceObject2 tag.
struct Place {
  bool ok = false;
  bool move = false, has_char = false, has_matrix = false, has_ratio = false, has_name = false, has_clip = false;
  uint16_t depth = 0, character = 0, ratio = 0, clip = 0;
  Matrix matrix;
  Bytes colour;  // (its colour transform as it was, or empty)
  std::string name;
  Bytes handlers;  // (clip actions as they were, or empty)
};
Place ReadPlace(const uint8_t* data, const Tag& tag) {
  Place p;
  if (tag.size < 3) return p;
  size_t at = tag.body;
  const uint8_t flags = data[at++];
  p.move = flags & 1;
  p.depth = U16(data + at);
  at += 2;
  if ((p.has_char = flags & 2)) p.character = U16(data + at), at += 2;
  if ((p.has_matrix = flags & 4)) {
    BitReader in{data, at};
    p.matrix = ReadMatrix(in);
    at = in.at;
  }
  if (flags & 8) {
    BitReader in{data, at};
    const bool add = in.U(1), multiply = in.U(1);
    const int n = int(in.U(4));
    in.U(n * 4 * (int(add) + int(multiply)) > 0 ? 0 : 0);
    for (int i = 0; i < 4 * (int(add) + int(multiply)); ++i) in.U(n);
    const size_t end = in.Align();
    p.colour.assign(data + at, data + end);
    at = end;
  }
  if ((p.has_ratio = flags & 0x10)) p.ratio = U16(data + at), at += 2;
  if ((p.has_name = flags & 0x20)) {
    while (at < tag.end() && data[at]) p.name.push_back(char(data[at++]));
    ++at;
  }
  if ((p.has_clip = flags & 0x40)) p.clip = U16(data + at), at += 2;
  if (flags & 0x80) p.handlers.assign(data + at, data + tag.end()), at = tag.end();
  p.ok = at <= tag.end();
  return p;
}
Bytes WritePlace(const Place& p) {
  Bytes out;
  out.push_back(uint8_t((p.move ? 1 : 0) | (p.has_char ? 2 : 0) | (p.has_matrix ? 4 : 0) | (p.colour.empty() ? 0 : 8) |
                        (p.has_ratio ? 0x10 : 0) | (p.has_name ? 0x20 : 0) | (p.has_clip ? 0x40 : 0) |
                        (p.handlers.empty() ? 0 : 0x80)));
  Put16(out, p.depth);
  if (p.has_char) Put16(out, p.character);
  if (p.has_matrix) Append(out, WriteMatrix(p.matrix));
  Append(out, p.colour);
  if (p.has_ratio) Put16(out, p.ratio);
  if (p.has_name) PutText(out, p.name);
  if (p.has_clip) Put16(out, p.clip);
  Append(out, p.handlers);
  return out;
}
Place NewPlace(uint16_t depth, uint16_t character, const Matrix& matrix, const std::string& name) {
  Place p;
  p.ok = true;
  p.depth = depth;
  p.has_char = true;
  p.character = character;
  p.has_matrix = true;
  p.matrix = matrix;
  p.has_name = !name.empty();
  p.name = name;
  return p;
}

// ---- ActionScript 2 byte code ----

struct Value {
  enum Kind { kText, kInt, kBool, kNumber } kind;
  std::string text;
  int32_t number = 0;
  double real = 0;
  Value(const char* s) : kind(kText), text(s) {}
  Value(const std::string& s) : kind(kText), text(s) {}
  Value(int v) : kind(kInt), number(v) {}
  Value(bool v) : kind(kBool), number(v) {}
  Value(double v) : kind(kNumber), real(v) {}
};

enum : uint8_t {
  kPop = 0x17, kGetVariable = 0x1C, kSetVariable = 0x1D, kStop = 0x07, kAdd2 = 0x47, kEquals2 = 0x49, kNot = 0x12,
  kDefineLocal = 0x3C, kCallFunction = 0x3D, kNewObject = 0x40, kGetMember = 0x4E, kSetMember = 0x4F,
  kCallMethod = 0x52, kPush = 0x96, kJump = 0x99, kIf = 0x9D, kDefineFunction = 0x9B, kGoToLabel = 0x8C,
};

struct Script {
  Bytes code;
  Script& Op(uint8_t op) {
    code.push_back(op);
    return *this;
  }
  // The values are pushed in the order given: the last one ends on top.
  Script& Push(std::initializer_list<Value> values) {
    Bytes body;
    for (const Value& v : values) {
      switch (v.kind) {
        case Value::kText:
          body.push_back(0);
          PutText(body, v.text);
          break;
        case Value::kInt:
          body.push_back(7);
          Put32(body, uint32_t(v.number));
          break;
        case Value::kBool:
          body.push_back(5);
          body.push_back(uint8_t(v.number));
          break;
        case Value::kNumber: {
          uint64_t bits;
          std::memcpy(&bits, &v.real, 8);
          body.push_back(6);
          Put32(body, uint32_t(bits >> 32));
          Put32(body, uint32_t(bits));
          break;
        }
      }
    }
    code.push_back(kPush);
    Put16(code, uint32_t(body.size()));
    Append(code, body);
    return *this;
  }
  // a.b.c ... : the object a path names, on the stack.
  Script& Get(std::initializer_list<const char*> path) {
    bool first = true;
    for (const char* part : path) {
      Push({part});
      Op(first ? kGetVariable : kGetMember);
      first = false;
    }
    return *this;
  }
  Script& GoToLabel(const char* label) {
    code.push_back(kGoToLabel);
    Put16(code, uint32_t(std::strlen(label) + 1));
    PutText(code, label);
    return *this;
  }
  // function name(params) { body }   (name "": the function is left on the stack)
  Script& Function(const char* name, std::initializer_list<const char*> params, const Script& body) {
    Bytes head;
    PutText(head, name);
    Put16(head, uint32_t(params.size()));
    for (const char* p : params) PutText(head, p);
    Put16(head, uint32_t(body.code.size()));
    code.push_back(kDefineFunction);
    Put16(code, uint32_t(head.size()));
    Append(code, head);
    Append(code, body.code);
    return *this;
  }
  // if (the value on the stack is false) { body }
  Script& Unless(const Script& body) {
    code.push_back(kIf);
    Put16(code, 2);
    Put16(code, uint32_t(body.code.size()));
    Append(code, body.code);
    return *this;
  }
  // ExternalCall(name, arguments...): the movies' own function around ExternalInterface.call.
  // (push_arguments leaves the arguments on the stack, the last one first)
  Script& ExternalCall(const char* name, int arguments = 0, const Script* push_arguments = nullptr) {
    if (push_arguments) Append(code, push_arguments->code);
    Push({name, arguments + 1, "ExternalCall"});
    return Op(kCallFunction).Op(kPop);
  }
  // name();
  Script& Call(const char* name) {
    Push({0, name});
    return Op(kCallFunction).Op(kPop);
  }
};

// list.AddReference(new CListReference(<clip>, <clip>.<text...>)) - a slot of one of the menus' lists.
void AddReference(Script& s, const char* list, std::initializer_list<const char*> clip,
                  std::initializer_list<const char*> text_in_clip) {
  s.Get(clip);
  for (const char* part : text_in_clip) s.Push({part}).Op(kGetMember);
  s.Get(clip);
  s.Push({2, "CListReference"}).Op(kNewObject);
  s.Push({1, list}).Op(kGetVariable).Push({"AddReference"}).Op(kCallMethod).Op(kPop);
}

// ---- the movie as a whole ----

// Where the tags begin (after the header: signature, version, length, stage rectangle, frame rate and count).
size_t FirstTag(const uint8_t* data, size_t size) {
  if (size < 21 || std::memcmp(data, "GFX", 3) != 0) return 0;
  BitReader in{data, 8};
  const int n = int(in.U(5));
  in.U(n), in.U(n), in.U(n), in.U(n);
  const size_t at = in.Align() + 4;
  return at < size ? at : 0;
}

void Finish(Bytes& movie) {
  const uint32_t size = uint32_t(movie.size());
  movie[4] = uint8_t(size), movie[5] = uint8_t(size >> 8), movie[6] = uint8_t(size >> 16), movie[7] = uint8_t(size >> 24);
}

// A script tag's body with more code put before its closing End.
bool ExtendScript(const uint8_t* data, const Tag& tag, const Script& more, Bytes* body) {
  if (!tag.size || data[tag.end() - 1] != 0) return false;
  body->assign(data + tag.body, data + tag.end() - 1);
  Append(*body, more.code);
  body->push_back(0);
  return true;
}

std::vector<uint8_t> Fail(std::string* why, const char* reason) {
  if (why) *why = reason;
  return {};
}

}  // namespace

std::string MovieName(const uint8_t* data, size_t size) {
  const size_t first = FirstTag(data, size);
  Tag tag;
  if (!first || !ReadTag(data, size, first, &tag) || tag.code != kExporterInfo || tag.size < 11) return {};
  // (version, flags, bitmap and audio formats, then the name with its length before it)
  const size_t length = data[tag.body + 9];
  if (tag.body + 10 + length > tag.end()) return {};
  return std::string(reinterpret_cast<const char*>(data + tag.body + 10), length);
}

// ---- the main menu: a fifth button ----

std::vector<uint8_t> PatchMainMenu(const uint8_t* data, size_t size, std::string* why) {
  constexpr uint16_t kFourth = 44, kFifth = 36;       // (depths)
  constexpr double kRight = 18.0, kDown = 51.0;       // (the fifth button from the fourth, pixels)
  // The credit line: a text field of its own in the buttons' font, bottom right inside the safe area (the stage
  // is 1280 x 720; the field ends 64 from the right edge, its line 44 above the bottom), placed and faded with
  // the fourth button's tags, so it shows on the main menu only.
  constexpr uint16_t kCreditDepth = 35, kCreditText = 30010;
  constexpr double kCreditWidth = 320.0, kCreditX = 1216.0 - kCreditWidth, kCreditY = 652.0, kCreditSize = 16.0;
  static const char kCredit[] = "<p align=\"right\">By KaikoClanworth</p>";
  if (MovieName(data, size) != "XMDMenuMain") return Fail(why, "not the main menu's movie");
  const size_t first = FirstTag(data, size);
  Bytes out(data, data + first);
  bool script = false, named = false;
  int copies = 0;
  uint16_t font = 0;  // (0: the movie has no text field before the buttons - then no credit line)
  Tag tag;
  size_t at = first;
  for (; ReadTag(data, size, at, &tag); at = tag.end()) {
    if (tag.code == kEnd) break;
    const size_t whole = tag.end() - tag.at;
    if (tag.code == kPlaceObject2) {
      Place p = ReadPlace(data, tag);
      if (!p.ok) return Fail(why, "a placement that does not read");
      if (p.depth == kFifth || p.depth == kCreditDepth) return Fail(why, "the fifth button's depth is taken");
      Append(out, data + tag.at, whole);
      if (p.depth != kFourth) continue;
      if (font) {
        Place credit = p;
        if (credit.has_char) {
          // (the field: its box, then read-only / coloured / with a font, laid out / not selectable / HTML /
          // drawn from the font's outlines, as the movie's own fields)
          Bytes body;
          Put16(body, kCreditText);
          BitWriter rect;
          const int32_t x1 = Twips(kCreditWidth), y1 = Twips(kCreditSize + 8.0);
          const int bits = BitsFor({-40, x1, -40, y1});
          rect.U(uint32_t(bits), 5);
          rect.S(-40, bits), rect.S(x1, bits), rect.S(-40, bits), rect.S(y1, bits);
          Append(body, rect.bytes);
          body.push_back(0x8D), body.push_back(0x33);
          Put16(body, font);
          Put16(body, uint32_t(Twips(kCreditSize)));
          body.push_back(0xE6), body.push_back(0xE6), body.push_back(0xE6), body.push_back(0xC8);  // (light grey, a little see-through)
          body.push_back(1);                                                                     // (aligned right)
          Put16(body, 0), Put16(body, 0), Put16(body, 0), Put16(body, 0);                        // (margins, indent, leading)
          body.push_back(0);                                                                     // (no variable)
          PutText(body, kCredit);
          PutTag(out, kDefineEditText, body);
          credit.character = kCreditText;
          credit.has_ratio = credit.has_clip = false;
          credit.handlers.clear();
        }
        credit.depth = kCreditDepth;
        if (credit.has_matrix) credit.matrix = At(kCreditX, kCreditY);
        if (credit.has_name) credit.name = "PCCredit";
        PutTag(out, kPlaceObject2, WritePlace(credit));
      }
      p.depth = kFifth;
      if (p.has_matrix) {
        // (below the fourth, leaning the other way, as the game's buttons alternate)
        p.matrix.tx += Twips(kRight);
        p.matrix.ty += Twips(kDown);
        p.matrix.r0 = -p.matrix.r0;
        p.matrix.r1 = -p.matrix.r1;
      }
      if (p.has_name) {
        if (p.name != "MainMenuNav4") return Fail(why, "depth 44 is not the fourth button");
        p.name = "MainMenuNav5";
        named = true;
      }
      PutTag(out, kPlaceObject2, WritePlace(p));
      ++copies;
    } else if (tag.code == kRemoveObject2 && tag.size == 2 && U16(data + tag.body) == kFourth) {
      Append(out, data + tag.at, whole);
      Bytes body;
      Put16(body, kFifth);
      PutTag(out, kRemoveObject2, body);
      if (font) {
        Bytes credit;
        Put16(credit, kCreditDepth);
        PutTag(out, kRemoveObject2, credit);
      }
    } else if (tag.code == kDoAction && !script && Holds(data + tag.body, tag.size, "MainMenuNav4") &&
               Holds(data + tag.body, tag.size, "CListReference")) {
      Script more;
      AddReference(more, "mMainListView", {"_root", "MainMenuNav5"}, {"MainMenuNavText", "ItemTitle"});
      Bytes body;
      if (!ExtendScript(data, tag, more, &body)) return Fail(why, "the list's script does not end as expected");
      PutTag(out, kDoAction, body);
      script = true;
    } else {
      // (the buttons' font: the first text field of the movie is a button's, its font follows the box and flags)
      if (tag.code == kDefineEditText && !font && tag.size > 8) {
        BitReader in{data, tag.body + 2};
        const int n = int(in.U(5));
        in.U(n), in.U(n), in.U(n), in.U(n);
        const size_t flags = in.Align();
        if (flags + 4 <= tag.end() && (data[flags] & 1)) font = U16(data + flags + 2);
      }
      Append(out, data + tag.at, whole);
    }
  }
  if (!script) return Fail(why, "the list's script was not found");
  if (!named || copies < 3) return Fail(why, "the fourth button was not found");
  Append(out, data + at, size - at);
  Finish(out);
  return out;
}

// ---- Options: a sixth entry and the Graphics page ----

namespace {

// The new characters' numbers (the movie's own stop below 200).
constexpr uint16_t kRowText = 30001, kRow = 30002, kPage = 30003, kKeys = 30004;

// The list's layout with six entries: where the first sits in Settings_MC and the step to the next (the disc's
// five: -47.65 and 85).
constexpr double kNavFirst = -77.65, kNavStep = 74.0;

// The Graphics page, in its own coordinates (the page sits where the Video page does): the panel is the Video
// page's, the rows run down its left side.
constexpr double kPageX = 638.45, kPageY = 363.90;
constexpr double kRowX = -236.0, kRowFirstY = -172.0, kRowStep = 50.0, kRowScale = 1.5;
constexpr double kRowTextWidth = 300.0;  // (the text field of a row, in its own units: 207.85 on the Game page)

std::string RowName(int row) { return "SelectionBox_MC" + std::to_string(row); }

// The key handlers of the Graphics page (the invisible button's actions).
Bytes GraphicsKeys() {
  struct Key {
    uint16_t condition;  // (the key's code << 9)
    Script script;
  };
  // _root.mGraphicsListView.miCurrentItem, on the stack
  const auto current = [](Script& s) { s.Get({"_root", "mGraphicsListView", "miCurrentItem"}); };
  // up / down: the list's own previous / next, as on the game's pages
  const auto move = [](const char* method) {
    Script inner;
    inner.Push({true, 1}).Get({"_root", "mGraphicsListView"}).Push({method}).Op(kCallMethod).Op(kPop);
    Script s;
    s.Get({"ModListener", "IsCTRL"}).Push({false}).Op(kEquals2).Op(kNot).Unless(inner);
    s.Push({0}).Get({"ModListener"}).Push({"FalseAllMod"}).Op(kCallMethod).Op(kPop);
    return s;
  };
  // a row's value is to change: PCGraphicsLeft / PCGraphicsRight / PCGraphicsPress(row). (The movies' ExternalCall
  // hands on the name and one argument, no more.)
  const auto change = [&](const char* callback, const char* sound) {
    Script s;
    s.Call(sound);
    Script arguments;
    current(arguments);
    s.ExternalCall(callback, 1, &arguments);
    return s;
  };
  // back: to the list, as the game's pages go (its own CollapseSubMenu puts the button legend back)
  Script back;
  back.Call("TriggerSelectSound").GoToLabel("SettingsHold").ExternalCall("PCGraphicsClose").ExternalCall("CollapseSubMenu");
  const Key keys[] = {
      {14 << 9, move("OnPrevious")},                              // up
      {15 << 9, move("OnNext")},                                  // down
      {1 << 9, change("PCGraphicsLeft", "TriggerNavSound")},      // left
      {2 << 9, change("PCGraphicsRight", "TriggerNavSound")},     // right
      {13 << 9, change("PCGraphicsPress", "TriggerSelectSound")}, // A / Enter
      {8 << 9, back},                                             // B / Backspace
  };
  Bytes out;
  const size_t count = sizeof(keys) / sizeof(keys[0]);
  for (size_t i = 0; i < count; ++i) {
    Bytes actions = keys[i].script.code;
    actions.push_back(0);
    Put16(out, i + 1 == count ? 0 : uint32_t(4 + actions.size()));
    Put16(out, keys[i].condition);
    Append(out, actions);
  }
  return out;
}

// The script of the Graphics frame: the page's list, as the Game page makes its own.
Script GraphicsScript(int rows) {
  Script s;
  // function AddGraphicsListViewItem(itemName) { mGraphicsListView.AddItem(new CTextBoxListViewItem(itemName, true)); }
  {
    Script body;
    body.Push({true}).Push({"itemName"}).Op(kGetVariable).Push({2, "CTextBoxListViewItem"}).Op(kNewObject);
    body.Push({1, "mGraphicsListView"}).Op(kGetVariable).Push({"AddItem"}).Op(kCallMethod).Op(kPop);
    s.Function("AddGraphicsListViewItem", {"itemName"}, body);
  }
  // function SetGraphicsItem(itemName, index) { mGraphicsListView.mItems[index].msName = itemName;
  //                                             mGraphicsListView.UpdateText(); }
  {
    Script body;
    body.Get({"mGraphicsListView", "mItems"}).Push({"index"}).Op(kGetVariable).Op(kGetMember);
    body.Push({"msName"}).Push({"itemName"}).Op(kGetVariable).Op(kSetMember);
    body.Push({0, "mGraphicsListView"}).Op(kGetVariable).Push({"UpdateText"}).Op(kCallMethod).Op(kPop);
    s.Function("SetGraphicsItem", {"itemName", "index"}, body);
  }
  s.Op(kStop);
  s.Push({"mGraphicsListView", 0, "CTextBoxListView"}).Op(kNewObject).Op(kDefineLocal);
  const auto set = [&s](const char* member, Value value) {
    s.Push({"mGraphicsListView"}).Op(kGetVariable).Push({member, value}).Op(kSetMember);
  };
  set("msEmptyText", "");
  set("mbOnFocusScale", true);
  set("miOnFocusInitScale", int(std::lround(kRowScale * 100)));
  set("miOnFocusScale", int(std::lround(kRowScale * 115)));
  for (int row = 1; row <= rows; ++row) {
    const std::string name = RowName(row);
    AddReference(s, "mGraphicsListView", {"_root", "SettingsGraphics_MC", name.c_str()}, {"ItemTitle"});
  }
  {
    Script body;
    body.Call("TriggerNavSound");
    s.Push({"mGraphicsListView"}).Op(kGetVariable).Push({"mASOnItemChanged"}).Function("", {}, body).Op(kSetMember);
  }
  s.ExternalCall("PCGraphicsPopulate");
  s.Push({0, "mGraphicsListView"}).Op(kGetVariable).Push({"UpdateText"}).Op(kCallMethod).Op(kPop);
  s.Push({0, 1, "mGraphicsListView"}).Op(kGetVariable).Push({"OnUpdate"}).Op(kCallMethod).Op(kPop);
  return s;
}

}  // namespace

double OptionsButtonY(int index) { return kNavFirst + kNavStep * index; }

std::vector<uint8_t> PatchOptions(const uint8_t* data, size_t size, std::string* why, unsigned parts) {
  if (MovieName(data, size) != "XMDMenuOptions") return Fail(why, "not the Options movie");
  const size_t first = FirstTag(data, size);
  const int rows = kGraphicsRows;

  // First pass: the pieces of the Game page the Graphics page is made of.
  Tag list_sprite{}, game_row{}, game_page{}, game_keys{}, row_text{}, row_text_settings{}, keys_place{};
  uint16_t row_text_id = 0, game_keys_id = 0, panel = 0, title = 0;
  Place title_place;
  Tag tag;
  size_t end_tag = 0;
  uint32_t frames = 0;
  for (size_t at = first; ReadTag(data, size, at, &tag); at = tag.end()) {
    if (tag.code == kEnd) {
      end_tag = tag.at;
      break;
    }
    if (tag.code == kShowFrame) ++frames;
    const uint8_t* body = data + tag.body;
    if (tag.code == kDefineSprite && Holds(body, tag.size, "Settings_Nav_MC5")) list_sprite = tag;
    if (tag.code == kDefineSprite && !game_row.size && Holds(body, tag.size, "ToggleBox_MC") &&
        Holds(body, tag.size, "ItemTitle")) {
      game_row = tag;
      Tag inner;
      for (size_t in = tag.body + 4; ReadTag(data, tag.end(), in, &inner) && inner.code != kEnd; in = inner.end()) {
        if (inner.code != kPlaceObject2) continue;
        const Place p = ReadPlace(data, inner);
        if (p.ok && p.name == "ItemTitle" && p.has_char) row_text_id = p.character;
      }
    }
    if (tag.code == kDefineSprite && !game_page.size && Holds(body, tag.size, "SelectionBox_MC1")) {
      game_page = tag;
      Tag inner;
      for (size_t in = tag.body + 4; ReadTag(data, tag.end(), in, &inner) && inner.code != kEnd; in = inner.end()) {
        if (inner.code != kPlaceObject2) continue;
        const Place p = ReadPlace(data, inner);
        if (!p.ok || !p.has_char) continue;
        if (p.depth == 1) panel = p.character;
        if (p.name == "XMD_MENU_Titles_SubRight_MC") title = p.character, title_place = p;
      }
    }
    if (tag.code == kDefineButton2 && Holds(body, tag.size, "mGameListView")) {
      game_keys = tag;
      game_keys_id = U16(body);
    }
    if (tag.code == kPlaceObject2 && game_keys_id) {
      const Place p = ReadPlace(data, tag);
      if (p.ok && p.has_char && p.character == game_keys_id) keys_place = tag;
    }
  }
  // (the row's text field and its font settings come before the row: a second look, now that its number is known)
  for (size_t at = first; ReadTag(data, size, at, &tag) && tag.code != kEnd; at = tag.end()) {
    if (tag.size < 2 || U16(data + tag.body) != row_text_id) continue;
    if (tag.code == kDefineEditText) row_text = tag;
    if (tag.code == kCsmTextSettings) row_text_settings = tag;
  }
  if (!end_tag || !list_sprite.size || !game_row.size || !game_page.size || !game_keys.size || !row_text.size ||
      !keys_place.size || !panel || !title || !row_text_id) {
    return Fail(why, "the Options movie is not laid out as expected");
  }
  if (frames != U16(data + first - 2)) return Fail(why, "the frame count does not match");

  Bytes out(data, data + first);
  bool script = false;
  for (size_t at = first; ReadTag(data, size, at, &tag); at = tag.end()) {
    if (tag.at == end_tag) break;
    const size_t whole = tag.end() - tag.at;
    if (tag.at == list_sprite.at && (parts & 1)) {
      // The list: six buttons, closer together.
      Bytes body(data + tag.body, data + tag.body + 4);
      Tag inner;
      bool sixth = false;
      for (size_t in = tag.body + 4; ReadTag(data, tag.end(), in, &inner); in = inner.end()) {
        Place p = inner.code == kPlaceObject2 ? ReadPlace(data, inner) : Place{};
        const bool button = p.ok && p.name.rfind("Settings_Nav_MC", 0) == 0 && p.name.size() == 16 && p.has_matrix;
        if (!button) {
          Append(body, data + inner.at, inner.end() - inner.at);
          if (inner.code == kEnd) break;
          continue;
        }
        const int index = p.name[15] - '1';
        p.matrix.ty = Twips(kNavFirst + kNavStep * index);
        PutTag(body, kPlaceObject2, WritePlace(p));
        if (index == 4) {
          p.depth = uint16_t(p.depth + 2);
          p.name = "Settings_Nav_MC6";
          p.matrix.ty = Twips(kNavFirst + kNavStep * 5);
          PutTag(body, kPlaceObject2, WritePlace(p));
          sixth = true;
        }
      }
      if (!sixth) return Fail(why, "the list's fifth button was not found");
      PutTag(out, kDefineSprite, body);
    } else if (tag.code == kDoAction && !script && (parts & 1) && Holds(data + tag.body, tag.size, "Settings_Nav_MC5")) {
      // The list's script: the sixth slot, then the texts once more (the game has filled the list by now).
      Script more;
      // function AddMainListViewItemDisabled(itemName) { mMainListView.AddItem(new CTextBoxListViewItem(itemName,
      // false)); } - the game calls it for "Difficulty" where the difficulty cannot be changed (the main menu) and
      // hides that button, but the movie on the disc has no such function: the entry was simply missing and the
      // list one short. With it the list always has the game's five entries, and "Graphics" is always the sixth.
      {
        Script body;
        body.Push({false}).Push({"itemName"}).Op(kGetVariable).Push({2, "CTextBoxListViewItem"}).Op(kNewObject);
        body.Push({1, "mMainListView"}).Op(kGetVariable).Push({"AddItem"}).Op(kCallMethod).Op(kPop);
        more.Function("AddMainListViewItemDisabled", {"itemName"}, body);
      }
      AddReference(more, "mMainListView", {"_root", "Settings_MC", "Settings_Nav_MC6", "XMD_MENU_TabButton_MC1"},
                   {"ItemTitle"});
      more.Push({0, "mMainListView"}).Op(kGetVariable).Push({"UpdateText"}).Op(kCallMethod).Op(kPop);
      more.Push({0, 1, "mMainListView"}).Op(kGetVariable).Push({"OnUpdate"}).Op(kCallMethod).Op(kPop);
      Bytes body;
      if (!ExtendScript(data, tag, more, &body)) return Fail(why, "the list's script does not end as expected");
      PutTag(out, kDoAction, body);
      script = true;
    } else {
      Append(out, data + tag.at, whole);
    }
  }
  if (!script && (parts & 1)) return Fail(why, "the list's script was not found");

  // The new frame, after the movie's last: label "Graphics".
  // A row's text field: the Game page's, wider.
  if (parts & 2) {
    BitReader in{data, row_text.body + 2};
    const int n = int(in.U(5));
    const int32_t x0 = in.S(n), x1 = in.S(n), y0 = in.S(n), y1 = in.S(n);
    (void)x1;
    const size_t rest = in.Align();
    const int32_t wide = x0 + Twips(kRowTextWidth);
    BitWriter rect;
    const int bits = BitsFor({x0, wide, y0, y1});
    rect.U(uint32_t(bits), 5);
    rect.S(x0, bits), rect.S(wide, bits), rect.S(y0, bits), rect.S(y1, bits);
    Bytes body;
    Put16(body, kRowText);
    Append(body, rect.bytes);
    Append(body, data + rest, row_text.end() - rest);
    PutTag(out, kDefineEditText, body);
    if (row_text_settings.size) {
      Bytes settings(data + row_text_settings.body, data + row_text_settings.end());
      settings[0] = uint8_t(kRowText), settings[1] = uint8_t(kRowText >> 8);
      PutTag(out, kCsmTextSettings, settings);
    }
  }
  // A row: the Game page's (tick box + text) with the wider text.
  if (parts & 2) {
    Bytes body;
    Put16(body, kRow);
    Append(body, data + game_row.body + 2, 2);
    Tag inner;
    for (size_t in = game_row.body + 4; ReadTag(data, game_row.end(), in, &inner); in = inner.end()) {
      Place p = inner.code == kPlaceObject2 ? ReadPlace(data, inner) : Place{};
      if (p.ok && p.has_char && p.character == row_text_id) {
        p.character = kRowText;
        PutTag(body, kPlaceObject2, WritePlace(p));
      } else {
        Append(body, data + inner.at, inner.end() - inner.at);
      }
      if (inner.code == kEnd) break;
    }
    PutTag(out, kDefineSprite, body);
  }
  // The page: the panel (drawn turned a quarter, as on the game's pages: its width is the second number), the
  // rows, the title at the panel's top right.
  if (parts & 2) {
    Bytes body;
    Put16(body, kPage);
    Put16(body, 1);
    Matrix m = At(3.65, 4.25);
    m.scaled = true, m.sx = m.sy = 0;
    m.rotated = true, m.r0 = Fixed(1.069), m.r1 = Fixed(-1.086);
    Place panel_place = NewPlace(1, panel, m, "");
    PutTag(body, kPlaceObject2, WritePlace(panel_place));
    for (int row = 1; row <= rows; ++row) {
      PutTag(body, kPlaceObject2,
             WritePlace(NewPlace(uint16_t(4 + row * 10), kRow, At(kRowX, kRowFirstY + kRowStep * (row - 1), kRowScale),
                                 RowName(row))));
    }
    Place t = title_place;
    t.depth = uint16_t(4 + (rows + 1) * 10);
    t.matrix.tx = Twips(195.50);
    t.matrix.ty = Twips(-235.35);
    PutTag(body, kPlaceObject2, WritePlace(t));
    PutTag(body, kShowFrame, {});
    PutTag(body, kEnd, {});
    PutTag(out, kDefineSprite, body);
  }
  // The keys: the Game page's invisible button with other handlers.
  if (parts & 2) {
    const uint8_t* b = data + game_keys.body;
    const uint32_t actions = U16(b + 3);
    if (!actions || 3 + actions > game_keys.size) return Fail(why, "the Game page's button has no handlers");
    Bytes body;
    Put16(body, kKeys);
    Append(body, b + 2, 1 + actions);  // (its flags, the offset of the handlers, its shapes)
    Append(body, GraphicsKeys());
    PutTag(out, kDefineButton2, body);
  }
  // The frame itself: the last page leaves, this one comes.
  if (parts & 4) {
    Bytes depth;
    Put16(depth, 18);
    PutTag(out, kRemoveObject2, depth);
    Bytes code = GraphicsScript(rows).code;
    code.push_back(0);
    PutTag(out, kDoAction, code);
    Bytes label;
    PutText(label, "Graphics");
    PutTag(out, kFrameLabel, label);
    PutTag(out, kPlaceObject2, WritePlace(NewPlace(18, kPage, At(kPageX, kPageY), "SettingsGraphics_MC")));
    Place keys = ReadPlace(data, keys_place);
    keys.character = kKeys;
    keys.has_ratio = false;
    PutTag(out, kPlaceObject2, WritePlace(keys));
    PutTag(out, kShowFrame, {});
  }
  Append(out, data + end_tag, size - end_tag);
  // (one frame more, in the header)
  if (parts & 4) out[first - 2] = uint8_t(frames + 1), out[first - 1] = uint8_t((frames + 1) >> 8);
  Finish(out);
  return out;
}

}  // namespace xmd::menupatch
