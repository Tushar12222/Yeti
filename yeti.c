/***IMPORTS***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE
#include <sys/types.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>

/***MACROS***/

// macro that is used to print the current version of the editor
#define YETI_VERSION "0.0.1"

// macro to check if any ctrl+key combination was used 
#define CTRL_KEY(k) ((k) & 0x1f)

// defines one tab space
#define YETI_TAB_STOP 8

/***DATA***/

// struct to  store the text typed
typedef struct editorRow{
	int size; // stores the length of the text
	int rsize; // stores the size of the actual text to be rendered
	char* text; // holds a line of text
	char* render; // contains the actual text to be rendered
} erow;

// enum to represent the arrow keys
enum editorKey{
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN
};

// struct to store the original attributes of the terminal to help configure  the editor size
struct editorConfig{
	char* filename; // stores the filename of the current file open in the editor
	int cx, cy; // stores the position of the cursor
	int rx; // holds cursor coordinate for the actual render
	int rowoff; // keeps track of the topmost row present on the current visible window
	int coloff; // keeps track of the leftmost column present on the current visible window
	int screenrows; // stores the height of the terminal
	int textrows; // store the no. of rows that contain the  text
	erow* row; // a pointer in which each item holds one line of text and its length
	int screencols; // stores the width of the terminal
	struct termios orig; // stores the attributes of the original terminal
};

// state variables that holds the current state of the editor
struct editorConfig state;

/***TERMINAL***/

// function to print error (in case there is any) and exit the program
void die(const char* s){
	// tells the terminal to clear the screen and postion the cursor to the top-left
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);
	
	// prints out a description of the error based on the global errorno set by the failed function
	perror(s);

	// exits the program with status code 1 which indicates failure
	exit(1);
}

// function to restore the original attributes of the terminal on exit
void disableRawMode(){
	// setting the default attributes back before exiting
	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &state.orig) == -1) die("tcsetattr");
}

// function to enable raw mode
void enableRawMode(){
	// get the attributes of the original terminal
	if(tcgetattr(STDIN_FILENO, &state.orig) == -1) die("tcgetattr");

	// makes sure that when the program is exited, the terminal is restored to original
	atexit(disableRawMode);

	// copy the original terminal attributes to be modified
	struct termios modified = state.orig;
	
	// IXON -> turns off ctrl-s and ctrl-q
	// ICTRNL -> fixes ctrl-m
	modified.c_iflag &= ~(ICRNL | IXON);

	// OPOST -> turns off output processing
	modified.c_oflag &= ~(OPOST);

	// turns off miscellaneous flags
	modified.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	modified.c_cflag |= (CS8);
	
	// ECHO -> stops echoing on terminal
	// ICANON -> removes the input buffer so you need not press enter to feed input to the program
	// ISIG -> turns off ctrl-c and ctrl-z
	// IEXTEN -> turns off ctrl-v
	modified.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	
	// sets  a timeout so that read() returns if it does not get any input for 100ms
	modified.c_cc[VMIN] = 0;
	modified.c_cc[VTIME] = 1;

	// setting the changes to the terminal
	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &modified) == -1) die("tcsetattr");

}


// func that reads each keypress
int editorReadKey(){
	// variables to store the response and the character respectively
	int nread;
	char c;

	// check if we were able to read the byte
	while((nread = read(STDIN_FILENO, &c, 1)) != 1){
		// if not exit the program
		if(nread == -1 && errno != EAGAIN) die("read");
	}
	
	// trying to check if arrow keys are useed since they are represented by 3 bytes and start as an escape sequence, followed by ']' and then A or B or C or D
	if(c == '\x1b'){
		char seq[3];

		if(read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if(read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if(seq[0] == '['){
			switch(seq[1]){
				case 'A': return ARROW_UP;
				case 'B': return ARROW_DOWN;
				case 'C': return ARROW_RIGHT;
				case 'D': return ARROW_LEFT;
			}
		}

		return '\x1b';
	} else { 
		return c;
	}
}

// func to get the current position of  the cursor
int getCursorPosition(int* rows, int* cols){
	// character buffer to store the response of the current cursor position
	char buffer[32];


	unsigned int i = 0;
	
	// try to request the terminal for the current cursor position
	if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

	// read the response sent by the terminal into the char buffer
	while(i < sizeof(buffer) - 1){
		if(read(STDIN_FILENO, &buffer[i], 1) != 1) break;
		if(buffer[i] == 'R') break;
		i++;
	}
	
	// makes the character buffer into a string in C
	buffer[i] = '\0';
	
	// check for valid cursor position
	if(buffer[0] != '\x1b' || buffer[1] != '[') return -1;
	
	//convert the response into numbers
	if(sscanf(&buffer[2], "%d;%d", rows, cols) != 2) return -1;
	
	return -1;
}

// func to get the terminal size
int getWindowSize(int* rows, int* cols){
	struct winsize ws;

	// tries to get the value of the terminal and assigns it to ws
	if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
		// try to reposition the cursor the bottom right of the screen
		if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;

		//if successfull, then we call the func
		return getCursorPosition(rows, cols);
	} else {
		// if successfull, assigns the  rows and cols respectively
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/***ROW OPERATIONS***/

// func converst the cx to rx based on the tab spaces present in the line
int editorRowCxToRx(erow* row, int cx){
	int rx = 0;
	for(int j = 0; j < cx; j++){
		if(row->text[j] == '\t') rx += (YETI_TAB_STOP - 1) - (rx % YETI_TAB_STOP);
		rx++;
	}

	return rx;
}

// func that converts tabs to spaces
void editorUpdateRow(erow* row){
	int tabs = 0;
	for(int j = 0; j < row->size; j++){
		if(row->text[j] == '\t') tabs++;
	}
	free(row->render);
	row->render = malloc(row->size + tabs*(YETI_TAB_STOP-1) + 1);

	int idx = 0;
	for(int j = 0; j < row->size; j++){
		if(row->text[j] == '\t'){
			row->render[idx++] = ' ';
			while(idx % YETI_TAB_STOP != 0) row->render[idx++] = ' ';
		} else {
			row->render[idx++] = row->text[j];
		}
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

void editorAppendRow(char *s, size_t len){
	// reallocate space to store the new line of text being read
	state.row = realloc(state.row, sizeof(erow) * (state.textrows + 1));
	
	// get the position in the array to where the  new text will be inserted 
	int at = state.textrows;

	// set the length of the text typed to the state
	state.row[at].size = len;

	// allocate enough space to the pointer that is going to hold the text
	state.row[at].text = malloc(len + 1);

	// copy the text from the file to the state to display
	memcpy(state.row[at].text, s, len);

	// null end the text to make it a string
	state.row[at].text[len] = '\0';

	// actual text to be rendered
	state.row[at].render = NULL;

	// size of the actual text to be rendered
	state.row[at].rsize = 0;
	
	editorUpdateRow(&state.row[at]);

	// update the no. of rows that contain text in the state
	state.textrows++;
}

/***FILE  I/O***/

// func to read the file passed to be read into the editor
void editorOpen(char *filename){
	// clear previous filename held
	free(state.filename);

	// automatically allocates and stores the filename
	state.filename = strdup(filename);

	// opening file to read contents
	FILE *fp = fopen(filename, "r");
	
	// if the  file could not be opened throw an error
	if(!fp) die("fopen");
	
	// stores the line read from the file
	char *line = NULL;

	// used to store the memory allocated to the store the line
	size_t linecap = 0;

	// store the length of the line read
	ssize_t linelen;

	// if there is text in that line
	while((linelen = getline(&line, &linecap, fp)) != -1){
		// removes the newline character since our struct erow anyways points to a single line always
		while(linelen > 0  && (line[linelen-1] == '\n' || line[linelen-1] == '\r')) linelen--;
		editorAppendRow(line , linelen);	
	}
	free(line);
	fclose(fp);
}

/***APPEND BUFFER***/

// struct to hold data about the buffer
struct append_buffer{
	char* b; // stores the text
	int len; // stores the length of the string
};

#define APPENDBUF_INIT {NULL, 0} // macro constructor to initialize a new append buffer

// func to append a new string to the buffer
void appBuffAppend(struct append_buffer* ab, const char* s, int len){
	// realloc more memory to the current string to be able to append the new string to it
	char* new = (char*)realloc(ab->b, ab->len + len);
	
	// if the reallocation was not possible
	if(new == NULL) return;

	// copies the string to be appended to the newly allocated memory
	memcpy(&new[ab->len], s, len);
	
	// update the current buffer with the newly appended pointer and len
	ab->b = new;
	ab->len += len;
}

// func to free the current buffer
void appBuffFree(struct append_buffer* ab){
	free(ab->b);
}

/***OUTPUT***/

// handles scrolling 
void editorScroll(){

	state.rx = 0;

	// as long as the cursor is on a text line, call the convert function
	if(state.cy < state.textrows) state.rx = editorRowCxToRx(&state.row[state.cy], state.cx);

	// if the cursor is above the visible screen, the editor scrolls up to the cursor position 
	if(state.cy < state.rowoff) state.rowoff = state.cy;
	
	// if the cursor is at the bottom of the screen, the editor is scrolled down depending on the difference between cy and screenrows and we add once since the y in editorDraw loop starts off with 0
	if(state.cy >= state.rowoff + state.screenrows) state.rowoff = (state.cy - state.screenrows) + 1;
	
	// same thing as above but for the horizontal scrolling
	if(state.rx < state.coloff) state.coloff = state.rx;
	if(state.rx >= state.coloff + state.screencols) state.coloff = (state.rx - state.screencols) + 1;
}

// func to draw dash to the  begiinig of each row
void editorDrawRows(struct append_buffer* ab){
	for(int y=0; y < state.screenrows; y++){
		// used to display the  correct range of lines based on the scroll position
		int filerow = y + state.rowoff;

		// if file row happens to be greater than the number of text lines present then we just print the dash to the editor
		if(filerow >= state.textrows){
			// writing the version of the editor one-third below the top only when there is no text present in the file supplied to the editor 
			if(state.textrows == 0 && y == state.screenrows / 3){
				// stores the text to be printed
				char welcome[80];
			
				// store a specific format of the text into the variable
				int welcomelen = snprintf(welcome, sizeof(welcome), "Yeti ---> version %s", YETI_VERSION);
				// reducing the text if the terminal screen has less width
				if(welcomelen > state.screencols) welcomelen = state.screencols;

				// centering the text
				int padding = (state.screencols - welcomelen) / 2;
				if (padding){
					appBuffAppend(ab, "-", 1);
					padding--;
				}
				while(padding--)appBuffAppend(ab, " ", 1);

				// write the text to the buffer 
				appBuffAppend(ab, welcome, welcomelen);
			} else {
				//append to the buffer the dashes to be drawn
				appBuffAppend(ab, "-", 1);
			}
		} else {
			// get the size of the text to be written to the editor
			int len = state.row[filerow].rsize - state.coloff;
			
			// if there is no text, then we do not write anything to the screen
			if(len < 0) len = 0;

			// if the size of the text is bigger than that of the editor, we  only show the  text that can be accomodated
			if(len > state.screencols) len = state.screencols;

			// appending the text to the append buffer that is used to write to the screen
			appBuffAppend(ab, &state.row[filerow].render[state.coloff], len);
		}
	
		// clear the line to the right once the dash is drawn
		appBuffAppend(ab, "\x1b[K" , 3);
		
		// append to the buffer the newline characters
		appBuffAppend(ab, "\r\n", 2);
	}
}

// func to draw the status bar
void editorDrawStatusBar(struct append_buffer* ab){
	// this tells the terminal to invert the colors attribute to the text written after this call
	appBuffAppend(ab, "\x1b[7m",  4);

	// state buffer to store the filename if it exists and rstatus to show the current cursor line 
	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), "%.20s - %d lines", state.filename ? state.filename : "[No Name]", state.textrows);
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", state.cy + 1, state.textrows);
	if(len > state.screencols) len = state.screenrows;
	appBuffAppend(ab, status, len);

	// write spaces so the entire status bar turns white
	while(len < state.screencols){
		// write the current cursor line to the end of the status bar
		if(state.screencols - len == rlen){
			appBuffAppend(ab, rstatus, rlen);
			break;
		} else {
			appBuffAppend(ab, " ", 1);
			len++;
		}
	}
	appBuffAppend(ab, "\x1b[m", 3);
}

// func to clear the screen
void editorRefreshScreen(){
	// func to handle vertical scrolling
	editorScroll();

	// initialize an empty append buffer
	struct append_buffer ab = APPENDBUF_INIT;

	// hide cursor while re drawing to the screen
	appBuffAppend(&ab, "\x1b[?25l", 6);

	// tells the terminal to position the cursor at row:1 and col:1 (i.e top-left)
	appBuffAppend(&ab, "\x1b[H", 3);

	// call func to write dashes to the buffer
	editorDrawRows(&ab);

	editorDrawStatusBar(&ab);
	
	// buffer to store the position of the cursor in a specific format
	char buffer[32];

	// store the position of the cursor in the required format
	snprintf(buffer, sizeof(buffer), "\x1b[%d;%dH", (state.cy - state.rowoff) + 1, (state.rx - state.coloff) + 1);

	// store the position to in the buffer
	appBuffAppend(&ab, buffer, strlen(buffer));

	// show the cursor
	appBuffAppend(&ab, "\x1b[?25h", 6);

	// now do one big write to the screen with the help of the buffer
	write(STDOUT_FILENO, ab.b, ab.len);

	// free the buffer
	appBuffFree(&ab);
}


/***INPUT***/

//handles movement of cursor in the editor
void editorMoveCursor(int key){
	// handles horizontal movement of the cursor on a line
	erow* curr_row = (state.cy >= state.textrows) ? NULL : &state.row[state.cy];  

	// switch case to change the global state of the cursor
	switch(key){
		case ARROW_LEFT:
			if(state.cy != 0 && state.cx == 0){
				state.cy--;
				state.cx = state.row[state.cy].size-1;
			} else if(state.cx > 0) state.cx--;
			break;
		case ARROW_RIGHT:
			if(curr_row && state.cx == curr_row->size-1 && state.cy < state.textrows){
				state.cy++;
				state.cx = 0;
			} else if(curr_row && state.cx != curr_row->size-1) state.cx++;
			break;
		case ARROW_UP:
			if(state.cy != 0) state.cy--;
			break;
		case ARROW_DOWN:
			if(state.cy != state.textrows) state.cy++;
			break;

	}
	
	curr_row = state.cy < state.textrows ? &state.row[state.cy] : NULL;
	if(curr_row && state.cx > curr_row->size-1) state.cx = curr_row->size-1;

}

// func to process keypress
void editorProcessKeypress(){
	int c = editorReadKey();

	switch (c){
		case CTRL_KEY('q'):
			// tells the terminal to clear the screen and postion the cursor to the top-left
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;
		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;
	}
}

/***INIT***/

// initializes the state of the editor
void initEditor(){
	// initial cursor position
	state.cx = 0;
	state.cy = 0;
	state.rx = 0;
	
	// initial no of rows containing text
	state.textrows = 0;

	// initial text
	state.row = NULL;

	// intial topmost row present on the visible screen
	state.rowoff = 0;

	// initial leftmost col visible on the scrrrn
	state.coloff = 0;

	// sets the screen size of the editor
	if(getWindowSize(&state.screenrows,  &state.screencols) == -1) die("getWindowSize");
	
	// leave the last line to display status bar
	state.screenrows -= 1;
}

int main(int argc, char *argv[]){
	// start the raw mode
	enableRawMode();
	
	// initialize the size of the editor
	initEditor();
	
	// read text from the file if supplied else open an empty editor
	if(argc >= 2) editorOpen(argv[1]);

	// loop to continuosly capture keystrokes
	while (1){
		// call the func to clear screen
		editorRefreshScreen();

		// call thee function to start processing keypresses
		editorProcessKeypress();
	}
	return 0;
}

