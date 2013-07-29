/*  BOSS

    A plugin load order optimiser for games that use the esp/esm plugin system.

    Copyright (C) 2012-2013    WrinklyNinja

    This file is part of BOSS.

    BOSS is free software: you can redistribute
    it and/or modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation, either version 3 of
    the License, or (at your option) any later version.

    BOSS is distributed in the hope that it will
    be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with BOSS.  If not, see
    <http://www.gnu.org/licenses/>.
*/

#include "network.h"
#include "error.h"
#include "parsers.h"
#include "streams.h"

#include <boost/log/trivial.hpp>

#include <git2.h>

#if _WIN32 || _WIN64
#   ifndef UNICODE
#       define UNICODE
#   endif
#   ifndef _UNICODE
#      define _UNICODE
#   endif
#   include "windows.h"
#   include "shlobj.h"
#endif
#define BUFSIZE 4096

using namespace std;

namespace fs = boost::filesystem;

namespace boss {

    bool RunCommand(const std::string& command, std::string& output) {
        HANDLE consoleWrite = NULL;
        HANDLE consoleRead = NULL;

        SECURITY_ATTRIBUTES saAttr;

        PROCESS_INFORMATION piProcInfo;
        STARTUPINFO siStartInfo;

        CHAR chBuf[BUFSIZE];
        DWORD dwRead;

        DWORD exitCode;

        //Init attributes.
        saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
        saAttr.bInheritHandle = TRUE;
        saAttr.lpSecurityDescriptor = NULL;

        //Create I/O pipes.
        if (!CreatePipe(&consoleRead, &consoleWrite, &saAttr, 0)) {
            BOOST_LOG_TRIVIAL(error) << "Could not create pipe for Subversion process.";
            throw error(error::subversion_error, "Could not create pipe for Subversion process.");
        }

        //Create a child process.
        BOOST_LOG_TRIVIAL(trace) << "Creating a child process.";
        ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));

        ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));
        siStartInfo.cb = sizeof(STARTUPINFO);
        siStartInfo.hStdError = consoleWrite;
        siStartInfo.hStdOutput = consoleWrite;
        siStartInfo.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        siStartInfo.wShowWindow = SW_HIDE;

        const int utf16Len = MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, NULL, 0);
        wchar_t * cmdLine = new wchar_t[utf16Len];
        MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, cmdLine, utf16Len);

        bool result = CreateProcess(NULL,
            cmdLine,     // command line
            NULL,          // process security attributes
            NULL,          // primary thread security attributes
            TRUE,          // handles are inherited
            0,             // creation flags
            NULL,          // use parent's environment
            NULL,          // use parent's current directory
            &siStartInfo,  // STARTUPINFO pointer
            &piProcInfo);  // receives PROCESS_INFORMATION

        delete [] cmdLine;

        if (!result) {
            BOOST_LOG_TRIVIAL(error) << "Could not create Subversion process.";
            throw error(error::subversion_error, "Could not create Subversion process.");
        }

        WaitForSingleObject(piProcInfo.hProcess, INFINITE);

        if (!GetExitCodeProcess(piProcInfo.hProcess, &exitCode)) {
            BOOST_LOG_TRIVIAL(error) << "Could not get Subversion process exit code.";
            throw error(error::subversion_error, "Could not get Subversion process exit code.");
        }

        if (!ReadFile(consoleRead, chBuf, BUFSIZE, &dwRead, NULL)) {
            BOOST_LOG_TRIVIAL(error) << "Could not read Subversion process output.";
            throw error(error::subversion_error, "Could not read Subversion process output.");
        }

        output = string(chBuf, dwRead);

        return exitCode == 0;
    }

    //Gets revision + date string.
    string GetRevision(const std::string& buffer) {
        string revision, date;
        size_t pos1, pos2;

        pos1 = buffer.rfind("Revision: ");
        if (pos1 == string::npos)
            return "";

        pos2 = buffer.find('\n', pos1);

        revision = buffer.substr(pos1+10, pos2-pos1-10);

        pos1 = buffer.find("Last Changed Date: ", pos2);
        pos2 = buffer.find(' ', pos1+19);

        date = buffer.substr(pos1+19, pos2-pos1-19);

        return revision + " (" + date + ")";
    }

    std::string UpdateMasterlist(const Game& game, std::vector<std::string>& parsingErrors) {

        //First need to decide how the masterlist is updated: using Git or Subversion?
        //Look at the update URL to decide.

        if (!boost::iends_with(game.URL(), ".git")) {  //Subversion

            string command, output, revision;
            //First check if the working copy is set up or not.
            command = g_path_svn.string() + " info \"" + game.MasterlistPath().string() + "\"";

            BOOST_LOG_TRIVIAL(trace) << "Checking to see if the working copy is set up or not for the masterlist at \"" + game.MasterlistPath().string() + "\"";
            bool success = RunCommand(command, output);

            revision = GetRevision(output);

            if (game.URL().empty()) {
                if (!revision.empty())
                    return revision;
                else
                    return "N/A";
            }

            if (!success) {
                BOOST_LOG_TRIVIAL(trace) << "Working copy is not set up, checking out repository.";
                //Working copy not set up, perform a checkout.
                command = g_path_svn.string() + " co --depth empty " + game.URL().substr(0, game.URL().rfind('/')) + " \"" + game.MasterlistPath().parent_path().string() + "\\.\"";
                if (!RunCommand(command, output)) {
                    BOOST_LOG_TRIVIAL(error) << "Subversion could not perform a checkout. Details: " << output;
                    throw error(error::subversion_error, "Subversion could not perform a checkout. Details: " + output);
                }
            }

            //Now update masterlist.
            BOOST_LOG_TRIVIAL(trace) << "Performing Subversion update of masterlist.";
            command = g_path_svn.string() + " update \"" + game.MasterlistPath().string() + "\"";
            if (!RunCommand(command, output)) {
                BOOST_LOG_TRIVIAL(error) << "Subversion could not update the masterlist. Details: " << output;
                throw error(error::subversion_error, "Subversion could not update the masterlist. Details: " + output);
            }

            while (true) {
                try {

                    //Now get the masterlist revision.
                    BOOST_LOG_TRIVIAL(trace) << "Getting the new masterlist version.";
                    command = g_path_svn.string() + " info \"" + game.MasterlistPath().string() + "\"";
                    if (!RunCommand(command, output)) {
                        BOOST_LOG_TRIVIAL(error) << "Subversion could not read the masterlist revision number. Details: " << output;
                        throw error(error::subversion_error, "Subversion could not read the masterlist revision number. Details: " + output);
                    }

                    BOOST_LOG_TRIVIAL(trace) << "Reading the masterlist version from the svn info output.";
                    revision = GetRevision(output);

                    //Now test masterlist to see if it parses OK.
                    BOOST_LOG_TRIVIAL(trace) << "Testing the new masterlist to see if it parses OK.";
                    YAML::Node mlist = YAML::LoadFile(game.MasterlistPath().string());

                    return revision;
                } catch (YAML::Exception& e) {
                    //Roll back one revision if there's an error.
                    BOOST_LOG_TRIVIAL(error) << "Masterlist parsing failed. Masterlist revision " + revision + ": " + e.what();
                    parsingErrors.push_back("Masterlist revision " + revision + ": " + e.what());
                    command = g_path_svn.string() + " update --revision PREV \"" + game.MasterlistPath().string() + "\"";
                    if (!RunCommand(command, output)) {
                        BOOST_LOG_TRIVIAL(error) << "Subversion could not update the masterlist. Details: " << output;
                        throw error(error::subversion_error, "Subversion could not update the masterlist. Details: " + output);
                    }
                }
            }
        } else {  //Git.
            /*  List of operations (porcelain commands shown, will need to implement using plumbing in the API though):

                1. Check if there is already a Git repository in the game's BOSS subfolder.

                    Since the masterlists will each be in the root of a separate repository, just check if there is a `.git` folder present.

                2a. If there is, compare its remote URL with the URL that BOSS is currently set to use.

                    The current remote can be gotten using `git config --get remote.origin.url`.


                3a. If the URLs are different, then update the remote URL to the one given by BOSS.

                    The remote URL can be changed using `git remote set-url origin <URL>`

                2b. If there isn't already a Git repo present, initialise one using the URL given (remembering to split the URL so that it ends in `.git`).

                    `git init`
                    `git remote add -f origin <URL>`


                3b. Now set up sparse checkout support, so that even if the repository has other files, the user's BOSS install only gets the masterlist added to it.

                    `git config core.sparseCheckout true`
                    `echo masterlist.yaml >> .git/info/sparse-checkout`

                4.  Now update the repository.

                    `git reset --hard HEAD` is required to undo any roll-backs that were done in the local repository.
                    `git pull origin master`

                5.  Now get the revision hash of the masterlist.

                    `git ls-files -s masterlist.yaml`. This also gives the file type and path though, so it's not a 1:1 match to what we really want.

                6.  Test the updated masterlist parsing, to make sure it isn't broken.

                7a. If it is broken, roll back the masterlist one revision, according to the remote history.

                    `git checkout HEAD~1 masterlist.yaml` to roll back one revision. HEAD stays in the same place, so need to increment the number if rolling back multiple times.

                8a. Now go back to step (5).

                7b. If it isn't broken, finish.
            */
            int error_code;
            git_repository * repo;
            git_remote * remote;
            git_config * cfg;

            //Checking for a ".git folder.
            if (fs::exists(game.MasterlistPath().parent_path() / ".git")) {
                //Repository exists. Open it.
                error_code = git_repository_open(&repo, game.MasterlistPath().parent_path().string().c_str());

                if (error_code) {
                    const git_error * error = giterr_last();
                    BOOST_LOG_TRIVIAL(error) << error->message;
                }

                //Now get remote info.
                error_code = git_remote_load(&remote, repo, "origin");

                if (error_code) {
                    const git_error * error = giterr_last();
                    BOOST_LOG_TRIVIAL(error) << error->message;
                }

                //Get the remote URL.
                const char * url = git_remote_url(remote);

                //Check if the URLs match.
                if (url != game.URL().c_str()) {
                    //The URLs don't match. Change the remote URL to match the one BOSS has.
                    error_code = git_remote_set_url(remote, game.URL().c_str());

                    if (error_code) {
                        const git_error * error = giterr_last();
                        BOOST_LOG_TRIVIAL(error) << error->message;
                    }
                }
            } else {
                //Repository doesn't exist. Set up a repository.
                error_code = git_repository_init(&repo, game.MasterlistPath().parent_path().string().c_str(), false);

                if (error_code) {
                    const git_error * error = giterr_last();
                    BOOST_LOG_TRIVIAL(error) << error->message;
                }

                //Now set the repository's remote.
                error_code = git_remote_create(&remote, repo, "origin", game.URL().c_str());

                if (error_code) {
                    const git_error * error = giterr_last();
                    BOOST_LOG_TRIVIAL(error) << error->message;
                }

                //Now set up the repository for sparse checkouts.
                error_code = git_repository_config(&cfg, repo);

                if (error_code) {
                    const git_error * error = giterr_last();
                    BOOST_LOG_TRIVIAL(error) << error->message;
                }

                error_code = git_config_set_bool(cfg, "core.sparseCheckout", true);

                if (error_code) {
                    const git_error * error = giterr_last();
                    BOOST_LOG_TRIVIAL(error) << error->message;
                }

                //Now add the masterlist file to the list of files to be checked out. We can actually just overwrite anything that was there previously, since it's only one file.

                boss::ofstream out(game.MasterlistPath().parent_path() / ".git/info/sparse-checkout");

                out << "masterlist.yaml";

                out.close();

            }

            //Now perform a hard reset to the repository HEAD, in case we rolled back any previous updates.



            //Finally, free memory.
            git_remote_free(remote);  //Not sure if git_repository_free calls this, so just being safe.
            git_repository_free(repo);


        }
    }
}
