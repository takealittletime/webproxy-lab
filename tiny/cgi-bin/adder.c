/*
 * adder.c - a minimal CGI program that adds two numbers together
 */
/* $begin adder */
#include "csapp.h"

int main(void) {
  FILE *html_file;
  char content[MAXLINE];

  html_file = fopen("cgi-bin/adder.html","r");
  if (html_file == NULL)
  {
    sprintf(content, "Content-type: text/html\r\n\r\n");
    sprintf(content + strlen(content), "<html><body><p>Error: adder.html not found</p></body></html>");
    printf("%s",content);
    fflush(stdout);
    exit(1);
  }

  printf("Connection: closer\r\n");
  printf("Connect-type: text/html\r\n\r\n");

  while (fgets(content,MAXLINE,html_file) != NULL)
  {
    printf("%s", content);
  }

  fflush(stdout);
  fclose(html_file);
  exit(0);
}
/* $end adder */
