/*
 * purr_catrt_game.h - the `game` catcall (purr_catcalls.h, plan6.md "The game surface"). The OS side of a game: who owns the screen and the buttons.
 */
#ifndef PURR_CATRT_GAME_H
#define PURR_CATRT_GAME_H

/* Registers the `game` catcall with the app runtime. Call once, before apps run. */
void purr_catrt_game_init(void);

#endif /* PURR_CATRT_GAME_H */
