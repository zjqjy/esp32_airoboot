/*
 * S4-C/D: 哭泣猫跳舞播放引擎 — LVGL 版 (实现见 cat_player_lvgl.c)
 * 语义移植自 S3-B/C cat_player.c: 单击切舞步 / 双击切猫 / 连点猫头趴地哭彩蛋
 */
#ifndef CAT_PLAYER_LVGL_H
#define CAT_PLAYER_LVGL_H

/* 创建 LVGL 控件与定时器后立即返回 (内部分锁, 须在 lvgl_port_init 之后调用) */
void cat_player_lvgl_start(void);

/* 全屏模式接管时暂停/恢复猫动画与触摸定时器 (S6-C 录音机用) */
void cat_player_lvgl_pause(void);
void cat_player_lvgl_resume(void);

#endif
