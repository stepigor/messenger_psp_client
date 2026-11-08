#include "markdown.h"
#include <string.h>
#include <ctype.h>
static void append(StMarkdown *m,const char *p,size_t n,unsigned style) {
    size_t room=sizeof(m->text)-1-m->length;if(n>room)n=room;
    memcpy(m->text+m->length,p,n);memset(m->style+m->length,(int)style,n);m->length+=n;m->text[m->length]=0;
}
static const char *closing(const char *p,const char *end,const char *delim,size_t n) {
    while(p+n<=end){if(*p=='\\'&&p+1<end){p+=2;continue;}if(!memcmp(p,delim,n))return p;p++;}return NULL;
}
static int word(unsigned char c){return c>=128||isalnum(c)||c=='_';}
static void parse(const char *p,const char *end,unsigned inherited,int depth,StMarkdown *out) {
    const char *begin=p;int sol=1;unsigned style=inherited;
    while(p<end&&out->length<sizeof(out->text)-1) {
        if(sol&&depth==0) {
            style=inherited;sol=0;
            while(p+1<end&&p[0]=='>'&&p[1]==' '){style|=ST_MD_QUOTE;p+=2;}
            const char *head=p;while(p<end&&*p=='#'&&p-head<6)p++;
            if(p>head&&p<end&&*p==' '){style|=ST_MD_BOLD|ST_MD_HEADING;p++;}else p=head;
            if(p+1<end&&strchr("-*+",*p)&&p[1]==' '){append(out,"\xe2\x80\xa2 ",4,style);p+=2;}
        }
        if(p>=end)break;
        if(*p=='\n'){append(out,p++,1,style);sol=1;continue;}
        if(*p=='\\'&&p+1<end&&strchr("\\`*_{}[]()<>#+-.!~|",p[1])){append(out,p+1,1,style|ST_MD_LITERAL);p+=2;continue;}
        if(*p=='`') {
            size_t n=p+2<end&&p[1]=='`'&&p[2]=='`'?3:p+1<end&&p[1]=='`'?2:1;const char *close=closing(p+n,end,p,n);
            if(close){const char *body=p+n;if(n==3){const char *line=memchr(body,'\n',(size_t)(close-body));if(line)body=line+1;}
                append(out,body,(size_t)(close-body),style|ST_MD_CODE);p=close+n;sol=p>body&&p[-1]=='\n';continue;}
        }
        if(depth<8&&*p=='[') {
            const char *label=closing(p+1,end,"](",2);
            if(label){const char *finish=closing(label+2,end,")",1);if(finish){parse(p+1,label,style|ST_MD_LINK,depth+1,out);p=finish+1;continue;}}
        }
        size_t n=0;unsigned added=0;
        if(*p=='*'||*p=='_') {
            n=p+2<end&&p[1]==*p&&p[2]==*p?3:p+1<end&&p[1]==*p?2:1;
            added=n==3?ST_MD_BOLD|ST_MD_ITALIC:n==2?ST_MD_BOLD:ST_MD_ITALIC;
            if(*p=='_'&&p>begin&&word((unsigned char)p[-1]))n=0;
        } else if(p+1<end&&p[0]=='~'&&p[1]=='~'){n=2;added=ST_MD_STRIKE;}
        if(n&&depth<8&&p+n<end&&!isspace((unsigned char)p[n])) {
            const char *close=closing(p+n,end,p,n);
            if(close&&close>p+n&&!isspace((unsigned char)close[-1])){parse(p+n,close,style|added,depth+1,out);p=close+n;continue;}
        }
        const char *next=p;st_utf8_next(&next);if(next>end)next=end;
        append(out,p,(size_t)(next-p),style);p=next;
    }
}
void st_markdown_parse(const char *text,StMarkdown *out) {
    memset(out,0,sizeof(*out));size_t n=strlen(text);if(n>ST_TEXT_MAX-1)n=ST_TEXT_MAX-1;
    parse(text,text+n,0,0,out);
}

static int reference_match(const char **text,char id[64],char kind) {
    const char *p=*text;if(p[0]!='<'||p[1]!=kind)return 0;
    size_t n=0;while(p[2+n]&&n<26&&strchr("0123456789ABCDEFGHJKMNPQRSTVWXYZ",p[2+n]))n++;
    if(n!=26||p[28]!='>')return 0;
    memcpy(id,p+2,26);id[26]=0;*text=p+29;return 1;
}
int st_mention_match(const char **text,char id[64]){return reference_match(text,id,'@');}
int st_channel_mention_match(const char **text,char id[64]){return reference_match(text,id,'#');}
