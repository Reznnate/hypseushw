/*
 *  homedir.h
 */

#ifndef HOMEDIR_H
#define HOMEDIR_H

#include <string> // STL strings, useful to prevent buffer overrun
#include "homedir.h"
#include "mpo_fileio.h"

using namespace std; // for STL string to compile without problems ...

class homedir
{
  public:
    // Constructor/destructors
    homedir();

    // Properties
    string get_homedir();
    string get_romdir();
    string get_ramdir();
    void set_lockhome(bool);
    void set_homedir(const string &s);
    void set_romdir(const string &s);
    void set_ramdir(const string &s);
    string get_romfile(const string &s);
    string get_ramfile(const string &s);
    string get_framefile(const string &s);
    void create_dirs(const string &s);

    // Searches homedir for a filename indicated by 'fileName' and if it doesn't
    // find it,
    //  searches the application directory for the same filename (if 'bFallback'
    //  is true).
    // Returns the found path,
    //  or an empty string if neither location yielded the file.
    string find_file(string fileName, bool bFallback = true);
    string find_romfile(string fileName, bool bFallback = true);

#ifdef __LIBRETRO__
    // Read-only asset locations (pics/, sound/) searched after the home
    // directory: the core's system folder, then the user's Daphne folder.
    void set_appdir(const string &s) { m_appdir = s; }
    void set_altdir(const string &s) { m_altdir = s; }
#endif

  private:
    void make_dir(const string &dir);

    // Private members
    bool m_lockhome; // Is POSIX running footloose and fancy free...
    string m_appdir; // Directory the app was launched from
    string m_homedir; //"Home" directory to search first (defaults to appdir)
    string m_romdir; // ROMS directory
    string m_ramdir; // RAM directory
#ifdef __LIBRETRO__
    string m_altdir;
#endif
};

extern homedir g_homedir; // our global game class.  Instead of having every
                          // .cpp file define this, we put it here.

// Path of a read-only asset (e.g. "pics/led0.bmp").  Upstream resolves these
// relative to the current directory, which is the game's folder (batch and
// Singe games) or the frontend's; the core's assets are in its system folder.
#ifdef __LIBRETRO__
#define LR_ASSET(p) g_homedir.find_file(p, true)
#else
#define LR_ASSET(p) string(p)
#endif

#endif
