/* img_name.c — Windows 文件名合法性（规则见 img_name.h） */
#include "img_name.h"
#include <stdio.h>
#include <string.h>

static bool bad_char(unsigned char c){
    if(c < 0x20) return true;              /* 控制字符：Win32 一律不接受 */
    switch(c){
    case '<': case '>': case ':': case '"': case '/':
    case '\\': case '|': case '?': case '*':
        return true;
    default:
        return false;
    }
}

/* UTF-16 码元数（NTFS/FAT 的上限都是 255 个码元，不是 255 字节）。
 * 坏 UTF-8 按 1 个码元计 —— 反正写入侧还会再校验一遍编码。 */
static size_t utf16_units(const char *s){
    size_t len = strlen(s), i = 0, n = 0;
    while(i < len){
        unsigned char c = (unsigned char)s[i];
        int seq = (c < 0x80) ? 1
                : ((c & 0xE0) == 0xC0) ? 2
                : ((c & 0xF0) == 0xE0) ? 3
                : ((c & 0xF8) == 0xF0) ? 4 : 1;
        if(i + (size_t)seq > len) seq = 1;
        unsigned cp = (seq == 1) ? c : (unsigned)(c & (0xFFu >> (seq + 1)));
        for(int k = 1; k < seq; k++) cp = (cp << 6) | ((unsigned char)s[i + (size_t)k] & 0x3Fu);
        i += (size_t)seq;
        n += (cp > 0xFFFF) ? 2 : 1;        /* 辅助平面：代理对占 2 个码元 */
    }
    return n;
}

static bool reserved_name(const char *name){
    char base[16];
    size_t i = 0;
    for(; name[i] && name[i] != '.'; i++){
        if(i + 1 >= sizeof base) return false;    /* 太长了，不可能是设备名 */
        char c = name[i];
        if(c >= 'a' && c <= 'z') c = (char)(c - 32);
        base[i] = c;
    }
    base[i] = 0;
    static const char *dev[] = { "CON", "PRN", "AUX", "NUL", NULL };
    for(int k = 0; dev[k]; k++) if(!strcmp(base, dev[k])) return true;
    if(i == 4 && (!strncmp(base, "COM", 3) || !strncmp(base, "LPT", 3)))
        return base[3] >= '1' && base[3] <= '9';
    return false;
}

img_name_status img_name_check(const char *name){
    if(!name || !*name || !strcmp(name, ".") || !strcmp(name, ".."))
        return IMG_NAME_EMPTY;
    for(const unsigned char *p = (const unsigned char *)name; *p; p++)
        if(bad_char(*p)) return IMG_NAME_BAD_CHAR;
    size_t n = strlen(name);
    if(name[n - 1] == '.' || name[n - 1] == ' ') return IMG_NAME_TRAILING;
    if(reserved_name(name)) return IMG_NAME_RESERVED;
    if(utf16_units(name) > 255) return IMG_NAME_TOO_LONG;
    return IMG_NAME_OK;
}

const char *img_name_status_id(img_name_status s){
    switch(s){
    case IMG_NAME_OK:       return "ok";
    case IMG_NAME_EMPTY:    return "empty";
    case IMG_NAME_BAD_CHAR: return "bad-char";
    case IMG_NAME_TRAILING: return "trailing";
    case IMG_NAME_RESERVED: return "reserved";
    case IMG_NAME_TOO_LONG: return "too-long";
    default:                return "?";
    }
}

const char *img_name_reason(img_name_status s, const char *name){
    static _Thread_local char buf[220];
    buf[0] = 0;
    switch(s){
    case IMG_NAME_OK:
        snprintf(buf, sizeof buf, "名字合法");
        break;
    case IMG_NAME_EMPTY:
        snprintf(buf, sizeof buf, "名字为空（不能是空、\".\" 或 \"..\"）");
        break;
    case IMG_NAME_BAD_CHAR: {
        unsigned char bad = 0;
        for(const unsigned char *p = (const unsigned char *)(name ? name : ""); *p; p++)
            if(bad_char(*p)){ bad = *p; break; }
        if(bad && bad < 0x20)
            snprintf(buf, sizeof buf, "名字含控制字符 0x%02X（Windows 不接受）", bad);
        else if(bad)
            snprintf(buf, sizeof buf, "名字含 Windows 不允许的字符 '%c'", bad);
        else
            snprintf(buf, sizeof buf, "名字含 Windows 不允许的字符");
        break;
    }
    case IMG_NAME_TRAILING:
        snprintf(buf, sizeof buf, "名字不能以 '.' 或空格结尾（Windows 会悄悄去掉，名字就对不上）");
        break;
    case IMG_NAME_RESERVED:
        snprintf(buf, sizeof buf, "\"%.64s\" 是 Windows 设备保留名"
                                  "（CON/PRN/AUX/NUL/COM1-9/LPT1-9，带扩展名也算）",
                 name ? name : "");
        break;
    case IMG_NAME_TOO_LONG:
        snprintf(buf, sizeof buf, "名字超过 255 个字符（NTFS/FAT 上限；工具不会截断）");
        break;
    default:
        snprintf(buf, sizeof buf, "名字不合法");
        break;
    }
    return buf;
}
