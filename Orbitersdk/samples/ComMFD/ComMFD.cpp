// ==============================================================
//                 ORBITER MODULE: ComMFD
//                  Part of the ORBITER SDK
//           Copyright (C) 2002-2003 Martin Schweiger
//                   All rights reserved
//
// ComMFD.cpp
// Reference implementation of generic user-defined MFD mode
// ==============================================================

#define STRICT
#define ORBITER_MODULE
#include <windows.h>
#include <stdio.h>
#include <queue>
#include <string> // for bool operator== (const string& lhs, const char* rhs);
#include <locale> // for locale save std::isspace() etc.
#include <sstream>
#include <iterator>

#include "orbitersdk.h"
#include "ComMFD.h"
#include "OrbiterSoundSDK40.h"


const int WAVE_INDEX_MAX = 4;

// ==============================================================
// Global variables

static struct {  // "Ascent MFD" parameters
	int mode;      // identifier for new MFD mode
} g_ComMFD;


// ==============================================================
// API interface

DLLCLBK void InitModule (HINSTANCE hDLL)
{
	static char *name = "Communication MFD";
	MFDMODESPECEX spec;
	spec.name    = name;
	spec.key     = OAPI_KEY_P;
	spec.context = NULL;
	spec.msgproc = ComMFD::MsgProc;
	g_ComMFD.mode = oapiRegisterMFDMode (spec);
}

DLLCLBK void ExitModule (HINSTANCE hDLL) {
	oapiUnregisterMFDMode (g_ComMFD.mode);
}

// We step through the tokens outside the MFD to keep tracking
// even if the MFD mode doesn't exist
DLLCLBK void opcPostStep (double simt, double simdt, double mjd)
{
	ComMFD* instance = ComMFD::GetInstance();
	if (instance) {
		instance->Read();
	}
}


// ==============================================================
// Global functions (callbacks for oapiOpenInputBox & helpers)

bool TxtInput (void *id, char* str, void* data) {
	return reinterpret_cast<ComMFD*>(data)->SetText(str);
}

// 'len' must hold enough space to include terminating zero!
static char * randomString (char* s, const int len)
{
	static const char alphanum[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	for (int i = 0; i < len; ++i) {
		s[i] = alphanum[rand() % (sizeof(alphanum) - 1)];
	}
	s[len] = '\0';
	return s;
}

static char * fullSet () {
	static char alphanum[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		                     " " // silence
	                         "0123456789"
		                     "." // decimal
	                         " MHz_request_docking";
	return alphanum;
}

int wordWrap ( const std::string& inputString,
                std::vector<std::string>& outputString,
                unsigned int lineLength )
{
	std::istringstream iss(inputString);
	std::string line;
	int lines = 0; // number of lines

	do
	{
		std::string word;
		iss >> word;

		if ( line.length() + word.length() > lineLength ) {
			outputString.push_back(line);
			line.clear();
			++lines;
		}
		line += word + " ";

	} while (iss);

	if ( !line.empty() ) {
		outputString.push_back(line);
	}

	return lines;
}

// ==============================================================
// Com MFD implementation

ComMFD::ComMFD (DWORD w, DWORD h, VESSEL *vessel, int key) :
	MFD2          (w, h, vessel),
	pVessel       (vessel),
	key           (key),
	wavNumber     ((key+1)%WAVE_INDEX_MAX),
	pitch         (100),
	voice         (0),
	focusFailError(false),
	orbiterSoundId(ConnectToOrbiterSoundDLL(vessel->GetHandle()))
{
	InitVoices();
	SetVoiceIndex(voice); // initializes voicePath
}

ComMFD::~ComMFD ()
{
	// Needed? Or does queue's d'tor the job
	if (!queue.empty()) {
		queue.erase( std::begin(queue), std::end(queue) );
	}
	if (!tokens.empty()) {
		tokens.erase( std::begin(tokens), std::end(tokens) );
	}
}


// ==============================================================
// Static variables and members

std::map<int,ComMFD*> ComMFD::Instances;
std::vector<std::string> ComMFD::voices; // { "cw", "icao", "en", "en-female" }

// round robin instance getter
ComMFD* ComMFD::GetInstance ()
{
	static int lastKey = -1;

	if (ComMFD::Instances.empty()) { return NULL; }

	auto it = ComMFD::Instances.upper_bound(lastKey);
	if (it == ComMFD::Instances.end()) {
		it = ComMFD::Instances.begin();
	}
	lastKey = it->first;

	return it->second;
}

// Initialize available "voices" (directory names)
void ComMFD::InitVoices()
{
	if (voices.empty())
	{
		// [[WIN32]]
		WIN32_FIND_DATA fileData;
		HANDLE hFile = FindFirstFile("./Sound/ComMFD/*", &fileData);
		if (hFile != INVALID_HANDLE_VALUE) {
			do {
				if (fileData.cFileName[0] != '.') { // skip ".", ".." and hidden directories (like ".svn")
					voices.push_back(fileData.cFileName);
				}
			} while (FindNextFile(hFile, &fileData));

			FindClose(hFile);
		}

		// [[c++17]]
		//std::string path = "./Sound/ComMFD";
		//for (const auto & entry : fs::directory_iterator(path)) {
		//	if (is_directory(entry)) {
		//		voices.push_back( entry.path().filename().string() );
		//	}
		//}
	}
}


// ==============================================================
// Members

// message parser
int ComMFD::MsgProc (UINT msg, UINT mfd, WPARAM wparam, LPARAM lparam)
{
	switch (msg)
	{
	case OAPI_MSG_MFD_OPENEDEX:
		{
			std::map<int, ComMFD*>::iterator it = ComMFD::Instances.find(mfd);
			if (it == ComMFD::Instances.end()) {
				MFDMODEOPENSPEC *ospec = (MFDMODEOPENSPEC*)wparam;
				ComMFD::Instances[mfd] = new ComMFD (ospec->w, ospec->h, (VESSEL*)lparam, mfd);
			}
			return (int)(ComMFD::Instances[mfd]);
		}

	case OAPI_MSG_MFD_CLOSED:
		{
			std::map<int, ComMFD*>::iterator it = ComMFD::Instances.find(mfd);
			if (it != ComMFD::Instances.end()) {
				ComMFD::Instances.erase(it); // Note: delete will be done by Orbiter core!
			}
			break;
		}
	}
	return 0;
}

bool ComMFD::ConsumeKeyBuffered (DWORD key)
{
	switch (key) {
	case OAPI_KEY_A:
		oapiOpenInputBox ("Enter Text:", TxtInput, 0, 20, (void*)this);
		return true;

	case OAPI_KEY_V:
		TxtInput(0, pVessel->GetName(), (void*)this);
		return true;

	case OAPI_KEY_F: {
		std::stringstream ss;
		ss << pVessel->GetNavRecvFreq(0) << "_MHz";
		TxtInput(0, &ss.str()[0], (void*)this);
		return true;
		}

	case OAPI_KEY_R: {
		std::stringstream ss;
		char rand[6];
		ss << randomString(rand, 6) << "_request_docking " << oapiGetSimTime();
		TxtInput(0, &ss.str()[0], (void*)this);
		return true;
		}

	case OAPI_KEY_U:
		if (pitch < 200) {
			++pitch;
			InvalidateDisplay();
			return true;
		}
		break;

	case OAPI_KEY_Z:
		pitch = 100;
		InvalidateDisplay();
		return true;

	case OAPI_KEY_D:
		if (pitch > 1) {
			--pitch;
			InvalidateDisplay();
			return true;
		}
		break;

	case OAPI_KEY_N:
		SetVoiceIndex(voice + 1);
		return true;
	}
	return false;
}

bool ComMFD::ConsumeButton (int bt, int event)
{
	if (!(event & PANEL_MOUSE_LBDOWN)) return false;
	static const DWORD btkey[] = { OAPI_KEY_A, OAPI_KEY_V, OAPI_KEY_F, OAPI_KEY_R, NULL, NULL,
		                           OAPI_KEY_U, OAPI_KEY_Z, OAPI_KEY_D, OAPI_KEY_N };
	if (bt < sizeof(btkey)/sizeof(DWORD)) return ConsumeKeyBuffered (btkey[bt]);
	else return false;
}

char *ComMFD::ButtonLabel (int bt)
{
	static char *label[] = { "TXT", "SGN", "FRQ", "RND", "\0", "\0",
		                     "+", "0", "-", "N" };
	return (bt < sizeof(label)/sizeof(char*) ? label[bt] : 0);
}

int ComMFD::ButtonMenu (const MFDBUTTONMENU **menu) const
{
	static const MFDBUTTONMENU mnu[] = {
		{ "Enter text", NULL            , 'A' },
		{ "Callsign"  , "of this vessel", 'V' },
		{ "Frequency" , "of 1st channel", 'F' },
		{ "Generate"  , "random text"   , 'R' },
		{ NULL        , NULL            , NULL},
		{ NULL        , NULL            , NULL},
		{ "Pitch up"  , NULL            , '+' },
		{ "Set Pitch" , "to 100%"       , '0' },
		{ "Pitch down", NULL            , '-' },
		{ "Next voice", NULL            , 'N' }
	};
	if (menu) *menu = mnu;
	return  sizeof(mnu)/sizeof(MFDBUTTONMENU);
}

bool ComMFD::AddTokenFileToPath (std::string& path, std::string& token)
{
	     if (token == ".") { token = "decimal"; }
	else if (token == " ") { token = "silence"; }
	else if (token.size()==1 && !isalnum(*(&token[0]))) {
		return false;
	}
	// const static std::locale loc;
	// std::transform(token.begin(), token.end(), token.begin(), ::tolower);
	path += token + ".wav";
	return true;
}

void ComMFD::SetVoiceIndex (size_t index)
{
	voice = index % voices.size();
	voicePath = "Sound\\ComMFD\\" + voices[voice] + "\\";
}

bool ComMFD::Update (oapi::Sketchpad *skp)
{
	if (!queue.empty())//&& IsPlaying(MyId, wavNumber))
	{
		// Display currently spoken tokens
		std::string dst("");
		int current = tokens.size() - queue.size()-1;
		for (int i = 0; i < tokens.size(); ++i) {
			if (i != current) { dst += tokens[i]; }
			else              { dst += "["; dst += tokens[i]; dst += "]"; }
		}
		Title (skp, &dst[0]);
	}

	// Current pitch
	std::ostringstream ss;
	ss << pitch << "%";
	skp->Text( GetWidth() - skp->GetTextWidth(ss.str().c_str()) - 5,
	           ceil(GetHeight() * 2 / 7),
			   ss.str().c_str(),
			   ss.str().length() );

	// Current voice
	auto txt = voices[voice].c_str();
	skp->Text( GetWidth() - skp->GetTextWidth(txt) - 5,
	           ceil(GetHeight() * 4 / 7),
	           txt,
	           voices[voice].length() );

	// Focus fail error message
	if (focusFailError)
	{
		auto x = 30;
		auto y = GetHeight() * 3 / 4;
		txt = "You have to switch focus";
		skp->Text(x, y, txt, strlen(txt));
		txt = "[F3] once to make it work!";
		skp->Text(x, y + LOWORD(skp->GetCharSize()), txt, strlen(txt));
	}
	return true;
}

bool ComMFD::Read ()
{
	if (!queue.empty() && !IsPlaying(orbiterSoundId, wavNumber))
	{
		InvalidateDisplay();
		std::string filePath(voicePath); // "Sound\\ComMFD\\[icao|en|en-femle|...]\\"

		if (AddTokenFileToPath(filePath, queue.front())) {
			auto a = RequestLoadVesselWave(orbiterSoundId, wavNumber, &filePath[0], INTERNAL_ONLY);
			auto b = PlayVesselWave(orbiterSoundId, wavNumber, NOLOOP, 255, int(pitch * 11025 / 100) );
			// We have to *change* focus... but from what vessel? and back...
			focusFailError = (!a || !b);
		}
		queue.erase(queue.begin());
	}

	return true;
}

bool ComMFD::SetText (const char *txt)
{
	tokens.clear();
	queue.clear();

	// Know commands (should be read from config file)
	static const char* inits[] = { "MHz", "request", "docking" };
	static const std::vector<std::string> commands(inits, std::end(inits));
	static const std::locale loc; // the non-std isspace() fails e.g. on 'esszett'

	bool isCW = (voices[voice] == "cw");

	std::string src(txt);
	if (isCW) {
		std::replace(src.begin(), src.end(), '_', ' ');
	}
	for (int i = 0, l = src.length(); i < l; ++i) {
		bool cmdFound = false;
		if (!isCW) { // No special commands in CW mode...
			// Are the next x characters a command?
			for (auto ci = commands.begin(); ci != commands.end(); ++ci) {
				std::size_t found = src.find(*ci, i);
				if (found == i) { // (found != std::string::npos && found == i)
					tokens.push_back(*ci);
					i += (*ci).length() - 1; // one less 'cause ++i in for-loop increment part!
					cmdFound = true;
					break;
				}
			}
		}
		// ...or 'simple' character
		if (!cmdFound) {
			if (isspace(src[i], loc)) {
				char prev = tokens.empty() ? 0 : tokens.back()[0];
				if (prev != ' ' && prev != '_') {
					tokens.push_back(" ");
				}
			}
			else if (isalnum(src[i], loc) || src[i] == '.') {
				tokens.push_back( std::string(sizeof(char), src[i]) );
			} else {
				tokens.push_back("_");
			}
		}
	}

	// Remove silence from start end end
	while (!tokens.empty() && (tokens.front() == " " || tokens.front() == "_") ) {
		tokens.erase(tokens.begin());
	}
	while (!tokens.empty() && (tokens.back() == " " || tokens.back() == "_") ) {
		tokens.pop_back();
	}

	// Add one 'space'. Helps display the last token ab[c] during play
	if (!tokens.empty()) {
		tokens.push_back("\t");
	}

	// Copy to string display member
	queue = tokens;

	return !tokens.empty();
}
