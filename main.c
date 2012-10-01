#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

/*****************************************************************************
 *
 ****************************************************************************/
#define DEFAULT_NAME_SIZE 8

/*****************************************************************************
 *
 ****************************************************************************/
typedef enum
{
  IDENTIFIER,
  OPERATOR,
  NUMBER
} ETokenClass;

/*****************************************************************************
 *
 ****************************************************************************/
typedef struct
{
  FILE* pFileHandle;
  char* pData;
  size_t uDataLength;
  size_t uIndex;
  char pLook;
  char pPeek;
  size_t uLine;
} CFileData;

/*****************************************************************************
 *
 ****************************************************************************/
static CFileData fileData =
{
  .pFileHandle = NULL,
  .pData = NULL,
  .uDataLength = 0,
  .uIndex = 0,
  .pLook = '\0',
  .pPeek = '\0',
  .uLine = 0
};

/*****************************************************************************
 *
 ****************************************************************************/
void* xMalloc( size_t uSize )
{
  char* pData = NULL;

  if( uSize < 1 )
  {
    printf( "Invalid length requested.\n" );
    return NULL;
  }

  pData = malloc( uSize );

  if( NULL == pData )
  {
    perror( "Error" );
    exit( EXIT_FAILURE );
  }

  return pData;
}

/*****************************************************************************
 *
 ****************************************************************************/
int ReadFile( char* pFileName )
{
  size_t uReadLength = 0;
  struct stat status;
  memset( &status, 0, sizeof( status ) );

  if( NULL == pFileName )
  {
    return EXIT_FAILURE;
  }

  if ( -1 == stat( pFileName, &status ) )
  {
    perror( "Error" );
    return EXIT_FAILURE;
  }

  fileData.uDataLength = status.st_size;

  if( NULL == ( fileData.pFileHandle = fopen( pFileName, "rb" ) ) )
  {
    perror( "Error" );
    return EXIT_FAILURE;
  }

  if( NULL == ( fileData.pData = xMalloc( fileData.uDataLength ) ) )
  {
    fclose( fileData.pFileHandle );
    return EXIT_FAILURE;
  }
  memset( fileData.pData, 0, fileData.uDataLength );

  uReadLength = fread( fileData.pData, 1, fileData.uDataLength, fileData.pFileHandle );

  if( uReadLength != fileData.uDataLength )
  {
    perror( "Error" );
    fclose( fileData.pFileHandle );
    free( fileData.pData );
    return EXIT_FAILURE;
  }

  fileData.uIndex = 0;
  fileData.pLook = fileData.pData[ 0 ];
  fileData.pPeek = fileData.pData[ 0 ];
  fileData.uLine = 0;

  return EXIT_SUCCESS;
}

/*****************************************************************************
 *
 ****************************************************************************/
void CloseFile( void )
{
  if( NULL != fileData.pFileHandle )
  {
    fclose( fileData.pFileHandle );
  }

  if( NULL != fileData.pData )
  {
    free( fileData.pData );
  }

  fileData.pFileHandle = NULL;
  fileData.pData = NULL;
  fileData.uDataLength = 0;
  fileData.uIndex = 0;
  fileData.pLook = '\0';
  fileData.pPeek = '\0';
  fileData.uLine = 0;
}

/*****************************************************************************
 *
 ****************************************************************************/
void NextChar( void )
{
  fileData.uIndex++;
  if( fileData.uIndex > fileData.uDataLength )
  {
    fileData.pLook = '\0';
    fileData.uIndex--;
  }
  else
  {
    fileData.pLook = fileData.pData[ fileData.uIndex ];
  }
}

/*****************************************************************************
 *
 ****************************************************************************/
void PeekChar( void )
{
  fileData.uIndex++;
  if( fileData.uIndex > fileData.uDataLength )
  {
    fileData.pPeek = '\0';
  }
  else
  {
    fileData.pPeek = fileData.pData[ fileData.uIndex ];
  }
  fileData.uIndex--;
}

/*****************************************************************************
 *
 ****************************************************************************/
void SkipSpace( void )
{
  while( isspace( fileData.pLook ) && fileData.pLook != '\n' )
  {
    NextChar();
  }
}

/*****************************************************************************
 *
 ****************************************************************************/
void NextLine( void )
{
  while( fileData.pLook != '\n' )
  {
    NextChar();
  }

  NextChar();
  fileData.uLine++;
}

/*****************************************************************************
 *
 ****************************************************************************/
char* GetName( void )
{
  char* pName = NULL;
  size_t uNameSize = DEFAULT_NAME_SIZE;
  size_t uNamePos = 0;

  pName = xMalloc( uNameSize + 1 );

  while(
      isdigit( fileData.pLook ) ||
      isalpha( fileData.pLook ) ||
      fileData.pLook == '.' ||
      fileData.pLook == '-'
      )
  {
    if( uNamePos - 1 == uNameSize )
    {
      uNameSize += DEFAULT_NAME_SIZE;
      pName = realloc( pName, uNameSize + 1 );

      if( pName == NULL )
      {
        perror( "Error" );
        exit( EXIT_FAILURE );
      }
    }

    pName[ uNamePos ] = fileData.pLook;
    NextChar();
    uNamePos++;
  }

  pName[ uNamePos ] = '\0';

  return pName;
}

/*****************************************************************************
 *
 ****************************************************************************/
int main( int argc, char *argv[] )
{
  if( argc < 2 )
  {
    printf( "Not enough arguments\n" );
    return EXIT_FAILURE;
  }

  if( EXIT_SUCCESS == ReadFile( argv[1]))
  {
    printf("%s", fileData.pData);
    CloseFile();
  }

  return EXIT_SUCCESS;
}
