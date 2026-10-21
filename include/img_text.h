/* img_text.h — 库内"人话"的中英双份（单一开关，CLI / App / JNI 共用）
 *
 * 边界（2026-10-21 与用户对齐，写进手册 §20）：
 *   · 机器可读的东西（命令名、参数名、错误码、检查项 id、verdict、reason）
 *     永远是 ASCII，App 拿这些去查【自己的文案表】——这是主路径。
 *   · 但信封里确实带了少量"人话"字段（note / detail / summary / 检查项 name），
 *     给 CLI 直接显示、给日志和远程排障用。既然带在库里，就必须能双语，
 *     不能只会中文。所以：中文串当 key，img_t() 按当前语言返回 zh/en。
 *   · 未找到 key 就原样返回（新增文案忘了翻译也不会崩，只是显示中文）。
 */
#ifndef IMG_TEXT_H
#define IMG_TEXT_H

#ifdef __cplusplus
extern "C" {
#endif

/* 语言：'z'=中文（默认） 'e'=英文。传 NULL 时读环境变量 IMG_LANG。 */
void img_lang_set(const char *code);      /* "zh"/"en"/"zh-CN"/"en-US" 都认 */
int  img_lang_en(void);                   /* 1=英文 */

/* 取文案：key 就是中文原文；英文环境下命中表则返回英文，否则原样返回 */
const char *img_t(const char *zh);

#ifdef __cplusplus
}
#endif
#endif
