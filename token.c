#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

/*****************************************************************************
 *
 ****************************************************************************/
typedef enum {
  TK_UNKNOWN,
  TK_IDENT,
  TK_NUM,
  TK_LPAREN,
  TK_RPAREN,
  TK_OP,
  TK_LABEL
} Token;

/*****************************************************************************
 *
 ****************************************************************************/
char *getline(FILE *stream)
  /*
   * Reads a complete line from an input stream, stores it and a NUL terminator
   * in an internal buffer, and returns a pointer to the start of the buffer.
   * Returns NULL if end-of-file occurs before any characters are read, or if
   * an I/O error occurs at any time, or if unable to allocate buffer memory (in
   * the latter cases, any characters read before the I/O error or allocation
   * failure are lost).  If the argument is NULL, frees the internal buffer and
   * returns NULL.
   *
   * A "complete line" consists of zero or more non-newline characters followed
   * by a newline, or one or more non-newline characters followed by EOF.
   */
{
  static char *buffer = NULL;
  static size_t bufsiz = 0;
  size_t length;
  int ch;

  length = 0;
  if (stream != NULL) {
    for (;;) {

      /* Ensure that the buffer has sufficient space for at least two
       * more characters: the one we're about to read plus a terminating
       * '\0'.  (If the stream ends with a newline-less line we'll need
       * only one more character, but the risk of wastage seems small.)
       */
      if (bufsiz - length < 2) {
        char *newbuf;
        bufsiz += (bufsiz == 0) ? 100 : bufsiz / 2;
        newbuf = realloc(buffer, bufsiz);
        if (newbuf == NULL) {
          length = 0;
          break;
        }
        buffer = newbuf;
      }

      /* Read the next character and check for end-of-file or error.
      */
      ch = getc(stream);
      if (ch == EOF) {
        if (ferror(stream))
          length = 0;
        break;
      }

      /* We got a real, live character.  Store it, and check for
       * end of line.
       */
      buffer[length++] = ch;
      if (ch == '\n')
        break;
    }
  }

  if (length > 0) {
    buffer[length] = '\0';
  }
  else {
    /* We're about to return NULL to indicate EOF, an error, or a memory
     * allocation failure, or else the argument was NULL to request a
     * cleanup.  In any case, it's reasonably likely the buffer won't be
     * needed again.
     */
    int errno_save = errno;
    free (buffer);
    buffer = NULL;
    bufsiz = 0;
    errno = errno_save;
  }

  return buffer;
}

/*****************************************************************************
 *
 ****************************************************************************/
void *pk_malloc(size_t s)
{
  void *res = malloc(s);

  if(!res && s) {
    fprintf(stderr, "malloc() failed\n");
    exit(EXIT_FAILURE);
  }

  return res;
}

/*****************************************************************************
 *
 ****************************************************************************/
char *pk_strdup(const char *s)
{
  char *new;

  if(!*s)
    return NULL;

  new = pk_malloc(strlen(s)+1);
  strcpy(new, s);

  return new;
}

/*****************************************************************************
 *
 ****************************************************************************/
size_t makeargv(char *string, char *argv[], int argvsize)
{
  char *p = string;
  int  i;
  size_t argc = 0;

  for(i = 0; i < argvsize; i++)
  {
    /* skip leading whitespace */
    while(isspace(*p))
    {
      p++;
    }

    if(*p != '\0')
    {
      argv[argc++] = p;
    }
    else
    {
      argv[argc] = 0;
      break;
    }

    /* scan over arg */
    while(*p != '\0' && !isspace(*p))
    {
      p++;
    }

    /* terminate arg: */
    if(*p != '\0' && i < argvsize-1)
    {
      *p++ = '\0';
    }
  }

  return argc;
}

/*****************************************************************************
 *
 ****************************************************************************/
Token token(char *str)
{
  if(isdigit(*str))
  {
    return TK_NUM;
  }

  switch( *str )
  {
    case '*':
    case '+':
    case '-':
    case '/':
      return TK_OP;
    case '(':
      return TK_LPAREN;
    case ')':
      return TK_RPAREN;
    case '#':
    case '$':
      return TK_NUM;
    default:
      break;
  }

  if(isalpha(*str))
  {
    if(':' == str[strlen(str)-1])
    {
      return TK_LABEL;
    }
    else
    {
      return TK_IDENT;
    }
  }

  return TK_UNKNOWN;
}

/*****************************************************************************
 *
 ****************************************************************************/
unsigned long getValue(char *str)
{
  int base = 0;
  size_t index = 0;
  unsigned long value = 0;

  /* #$100 */
  if('#' == str[0] && '$' == str[1])
  {
    base = 16;
    index = 2;
  }
  /* #100 */
  else if('#' == str[0] && isdigit(str[1]))
  {
    base = 10;
    index = 1;
  }
  /* $100 */
  else if('$' == str[0])
  {
    base = 16;
    index = 1;
  }
  /* 100 */
  else if(isdigit(str[0]))
  {
    base = 10;
    index = 0;
  }
  else
  {
    base = 0;
  }

  if(!base)
  {
    return -1;
  }

  value = strtoul(&str[index], NULL, base);

  return value;
}

/*****************************************************************************
 *
 ****************************************************************************/
int getIdent(char *str)
{
}

/*****************************************************************************
 *
 ****************************************************************************/
void handle_file(FILE *fh)
{
  size_t i;
  size_t ac;

  char *av[10];
  char *line;
  char *foo;

  while( (line = getline(fh)) )
  {
    foo = pk_strdup(line);
    ac = makeargv(foo, av, 10);

    for(i=0; i<ac; i++)
    {
      switch( token( av[i] ) )
      {
        case TK_NUM:
          printf("\'%s\' : TK_NUM = %lu\n", av[i], getValue(av[i]));
          break;
        case TK_IDENT:
          printf("\'%s\' : TK_IDENT\n", av[i]);
          break;
        case TK_LABEL:
          printf("\'%s\' : TK_LABEL\n", av[i]);
          break;
        case TK_OP:
          printf("\'%s\' : TK_OP\n", av[i]);
          break;
        case TK_LPAREN:
          printf("\'%s\' : TK_LPAREN\n", av[i]);
          break;
        case TK_RPAREN:
          printf("\'%s\' : TK_RPAREN\n", av[i]);
          break;
        case TK_UNKNOWN:
          printf("\'%s\' : TK_UNKNOWN\n", av[i]);
          break;
        default:
          break;
      }
    }

    free(foo);
  }
}

/*****************************************************************************
 *
 ****************************************************************************/
int main(int argc, char **argv)
{
  FILE *fh;

  if(argc < 2)
  {
    fprintf(stderr, "more args please.\n");
    return EXIT_FAILURE;
  }

  if(!(fh = fopen(argv[1], "rb")))
  {
    fprintf(stderr, "no suck file.\n");
    return EXIT_FAILURE;
  }

  handle_file(fh);
  fclose(fh);

  return EXIT_SUCCESS;
}
