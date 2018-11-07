#ifndef LINE_FILE_H_
#define LINE_FILE_H_

/*
 * A "line file" is a text file that you want to read line-by-line.
 *
 * As defined by RFC7730 (the only current user of this code), lines are
 * terminated by either CRLF or LF.
 * (...which is the same as saying "lines are terminated by LF.")
 */

#include <stddef.h>

struct line_file;

int lfile_open(const char *, struct line_file **);
void lfile_close();

int lfile_read(struct line_file *, char **);

const char *lfile_name(struct line_file *);
size_t lfile_offset(struct line_file *);

#endif /* LINE_FILE_H_ */