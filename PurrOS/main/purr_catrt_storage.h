/*
 * purr_catrt_storage.h - the `storage` catcall (plan9.md): an app's own data folder under the signed-in user.
 */
#ifndef PURR_CATRT_STORAGE_H
#define PURR_CATRT_STORAGE_H

/* Registers the `storage` catcall with the app runtime. Call once, after the filesystem is mounted. */
void purr_catrt_storage_init(void);

#endif /* PURR_CATRT_STORAGE_H */
