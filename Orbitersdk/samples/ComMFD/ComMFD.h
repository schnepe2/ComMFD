#ifndef __COMMFD_H
#define __COMMFD_H

#include <vector>
#include <map>
#include <string>


class ComMFD: public MFD2 {
public:
	ComMFD (DWORD w, DWORD h, VESSEL *vessel, int key);
	~ComMFD ();

	static int     MsgProc (UINT msg, UINT mfd, WPARAM wparam, LPARAM lparam);
	static ComMFD* GetInstance ();

	bool  ConsumeKeyBuffered (DWORD key);
	bool  ConsumeButton (int bt, int event);
	char* ButtonLabel (int bt);
	int   ButtonMenu (const MFDBUTTONMENU **menu) const;
	bool  Update (oapi::Sketchpad *skp);
	bool  SetText (const char *txt);
	bool  Read ();

private:
	static std::map<int,ComMFD*> Instances; ///< contains all instances of this MFD
	static std::vector<std::string> voices; ///< available "voices" (directory name)
	static void InitVoices ();              ///< initializes available "voices" (directory name)

	std::vector<std::string> queue;  ///< queue with tokens (left) to be played
	std::vector<std::string> tokens; ///< all tokens (for display)

	VESSEL* pVessel;        ///< Vessel this MFD is in
	int     key;            ///< mfd ID given at MsgProc(OAPI_MSG_MFD_OPENEDEX,...)
	int     wavNumber;      ///< wave number used by this MFD instance
	int     orbiterSoundId; ///< returned from ConnectToOrbiterSoundDLL()
	int     pitch;          ///< current pitch [1...200 percent]
	size_t  voice;          ///< current used voice (index ito voices)
	std::string  voicePath; ///< base path for current voice
	bool    focusFailError; ///< indicates that a focus-change might fix an issue

	void SetVoiceIndex (size_t voice);
	bool AddTokenFileToPath (std::string& path, std::string& token);
};

#endif //!__COMMMFD_H