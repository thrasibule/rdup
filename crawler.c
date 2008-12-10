/*
 * Copyright (c) 2005 - 2007 Miek Gieben
 * See LICENSE for the license
 *
 * Directory crawler
 */
#include "rdup.h"

extern gboolean opt_onefilesystem;
extern gboolean opt_nobackup;
extern time_t opt_timestamp;
extern gint opt_verbose;
extern GSList *pregex_list;

/* xattr.c */
uid_t read_attr_uid(char *path, uid_t u);
gid_t read_attr_gid(char *path, gid_t g);

static struct r_entry *
entry_dup(struct r_entry *f)
{
        struct r_entry *g;
        g = g_malloc(sizeof(struct r_entry));

        g->f_name       = g_strdup(f->f_name);
        g->f_name_size  = f->f_name_size;
        g->f_lnk	= f->f_lnk;
        g->f_uid        = f->f_uid;
        g->f_gid        = f->f_gid;
        g->f_mode       = f->f_mode;
	g->f_ctime      = f->f_ctime;
	g->f_size       = f->f_size;
	g->f_dev        = f->f_dev;
	g->f_rdev       = f->f_rdev;
	g->f_ino        = f->f_ino;
        return g;
}

static void
entry_free(struct r_entry *f)
{
	g_free(f->f_name);
	g_free(f);
}

/**
 * prepend path leading up to backup directory to the tree
 */
gboolean
dir_prepend(GTree *t, char *path)
{
	char *c;
	char *p;
	char *path2;
	size_t len;
	struct stat s;
	struct r_entry e;

	path2 = g_strdup(path);
	len   = strlen(path);

	/* add closing / */
	if (path2[len - 1] != DIR_SEP) {
		path2 = g_realloc(path2, len + 2);
		path2[len] = DIR_SEP;
		path2[len + 1] = '\0';
	}

	for (p = path2 + 1; (c = strchr(p, DIR_SEP)); p++) {
		*c = '\0';
		if (lstat(path2, &s) != 0) {
			msg(_("Could not stat path `%s\': %s"), path2, strerror(errno));
			return FALSE;
		}
		e.f_name      = path2;
		e.f_name_size = strlen(path2);
		e.f_uid       = s.st_uid;
		e.f_gid       = s.st_gid;
		e.f_ctime     = s.st_ctime;
		e.f_mode      = s.st_mode;
		e.f_size      = s.st_size;
		e.f_dev       = s.st_dev;
		e.f_rdev      = s.st_rdev;
		e.f_ino       = s.st_ino;
		e.f_lnk	      = 0;

		/* symlinks; also put the -> name in f_name */
		if (S_ISLNK(s.st_mode))
			e = *(sym_link(&e));

		g_tree_insert(t, (gpointer) entry_dup(&e), VALUE);
		*c = DIR_SEP;
		p = c++;
	}
	g_free(path2);
	return TRUE;
}

void
dir_crawl(GTree *t, GHashTable *linkhash, char *path)
{
	DIR 		*dir;
	struct dirent 	*dent;
	struct r_entry  *directory;
	char 		*curpath;
	gchar		*lnk;
	struct stat   	s;
	struct r_entry  pop;
	struct remove_path rp;
	dev_t 		current_dev;
	size_t 		curpath_len;

	/* dir stack */
	gint32 d = 0;
	gint32 dstack_cnt  = 1;
	struct r_entry **dirstack =
		g_malloc(dstack_cnt * D_STACKSIZE * sizeof(struct r_entry *));

	if (!(dir = opendir(path))) {
		/* non-dirs are also allowed, check for this, if it isn't give the error */
		if (access(path, R_OK) == 0) {
			g_free(dirstack);
			return;
		}
		msg(_("Cannot enter directory `%s\': %s"), path, strerror(errno));
		g_free(dirstack);
		return;
	}

	/* get device */
#ifdef HAVE_DIRFD
	if (fstat(dirfd(dir), &s) != 0) {
#else
	if (fstat(rdup_dirfd(dir), &s) != 0) {
#endif
		msg(_("Cannot determine holding device of the directory `%s\': %s"), path, 
				strerror(errno));
		closedir(dir);
		g_free(dirstack);
		return;
	}
	current_dev = s.st_dev;

	while((dent = readdir(dir))) {
		if (!strcmp(dent->d_name, ".") || 
				!strcmp(dent->d_name, ".."))
			continue;

		if (strcmp(path, DIR_SEP_STR) == 0)  {
			curpath = g_strdup_printf("%c%s", DIR_SEP, dent->d_name);
			curpath_len = strlen(curpath);
		} else {
			curpath = g_strdup_printf("%s%c%s", path, DIR_SEP, dent->d_name);
			curpath_len = strlen(curpath);
		}

		if (lstat(curpath, &s) != 0) {
			msg(_("Could not stat path `%s\': %s"), curpath, strerror(errno));
			g_free(curpath);
			continue;
		}

		if (strchr(curpath, '\n')) {
			msg(_("Newline (\\n) found in path `%s\', skipping"), curpath);
			g_free(curpath);
			continue;
		}

		if (S_ISREG(s.st_mode) || S_ISLNK(s.st_mode) || 
				S_ISBLK(s.st_mode) || S_ISCHR(s.st_mode) ||
				S_ISFIFO(s.st_mode) || S_ISSOCK(s.st_mode) ) {

			pop.f_name      = curpath;
			pop.f_name_size = curpath_len;
			pop.f_uid       = s.st_uid;
			pop.f_gid       = s.st_gid;
			pop.f_ctime     = s.st_ctime;
			pop.f_mode      = s.st_mode;
			pop.f_size      = s.st_size;
			pop.f_dev       = s.st_dev;
			pop.f_rdev      = s.st_rdev;
			pop.f_ino       = s.st_ino;
			pop.f_lnk	= 0;

			if (gfunc_regexp(pregex_list, curpath, curpath_len)) {
				g_free(curpath);
				continue;
			}

			if (opt_nobackup && !strcmp(dent->d_name, NOBACKUP)) {
				/* return after seeing .nobackup */
				if (opt_verbose > 0) {
					msg(_("%s found in '%s\'"), NOBACKUP, path);
				}
				/* remove all files found in this path */
				rp.tree = t;
				rp.len  = strlen(path);
				rp.path = path;
				g_tree_foreach(t, gfunc_remove_path, (gpointer)&rp);
				/* add .nobackup back in */
				g_tree_insert(t, (gpointer) entry_dup(&pop), VALUE);
				g_free(dirstack);
				closedir(dir);
				return;
			}

			/* hardlinks */
			if (s.st_nlink > 1) {
				if ((lnk = hard_link(linkhash, &pop))) {
					/* we got a match back - cannot use sym_link which does 
					 * a readlink */
					pop.f_size = strlen(pop.f_name);  /* old name length */
					lnk = g_strdup_printf("%s -> %s", pop.f_name, lnk);
					pop.f_lnk = 1;
					pop.f_name = lnk;
					pop.f_name_size = strlen(pop.f_name);
				}
			}
			if (S_ISLNK(s.st_mode)) 
				pop = *(sym_link(&pop));

			g_tree_insert(t, (gpointer) entry_dup(&pop), VALUE);
			g_free(curpath);
			continue;
		} else if(S_ISDIR(s.st_mode)) {
			/* one filesystem */
			if (opt_onefilesystem && s.st_dev != current_dev) {
				msg(_("Walking into different filesystem"));
				g_free(curpath);
				continue;
			}
			/* Exclude list */
			if (gfunc_regexp(pregex_list, curpath, curpath_len)) {
				g_free(curpath);
				continue;
			}

			dirstack[d] = g_malloc(sizeof(struct r_entry));
			dirstack[d]->f_name       = g_strdup(curpath); 
			dirstack[d]->f_name_size  = curpath_len;
			dirstack[d]->f_uid	  = s.st_uid;
			dirstack[d]->f_gid        = s.st_gid;
			dirstack[d]->f_ctime      = s.st_ctime;
			dirstack[d]->f_mode       = s.st_mode;
			dirstack[d]->f_size       = s.st_size;
			dirstack[d]->f_dev        = s.st_dev;
			dirstack[d]->f_rdev       = s.st_rdev;
			dirstack[d]->f_ino        = s.st_ino;
			dirstack[d]->f_lnk        = 0;

			if (d++ % D_STACKSIZE == 0) {
				dirstack = g_realloc(dirstack, 
						++dstack_cnt * D_STACKSIZE * 
						sizeof(struct r_entry *));
			}
			g_free(curpath);
			continue;
		} else {
			if (opt_verbose > 0) {
				msg(_("Neither file nor directory `%s\'"), curpath);
			}
			g_free(curpath);
		}
	}
	closedir(dir);

	while (d > 0) {
		directory = dirstack[--d]; 
		g_tree_insert(t, (gpointer) entry_dup(directory), VALUE);
		/* recurse */
		/* potentially expensive operation. Better would be to when we hit
		 * .nobackup to go up the tree and delete some nodes.... or not */
		dir_crawl(t, linkhash, directory->f_name);
		entry_free(directory);
	}
	g_free(dirstack);
	return;
}
