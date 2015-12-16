/*    UrBackup - Client/Server backup system
*    Copyright (C) 2011-2015 Martin Raiber
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU Affero General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************/

#include "FileBackup.h"
#include "ClientMain.h"
#include "server_status.h"
#include "server_log.h"
#include <assert.h>
#include "server_ping.h"
#include "database.h"
#include "../urbackupcommon/filelist_utils.h"
#include <algorithm>
#include "../urbackupcommon/os_functions.h"
#include "../urbackupcommon/file_metadata.h"
#include <sstream>
#include "create_files_index.h"
#include <time.h>
#include "snapshot_helper.h"
#include "server_dir_links.h"
#include "server_cleanup.h"
#include <stack>
#include <limits.h>
#include "FileMetadataDownloadThread.h"
#include "../utf8/utf8.h"

#ifndef NAME_MAX
#define NAME_MAX _POSIX_NAME_MAX
#endif

const unsigned int full_backup_construct_timeout=4*60*60*1000;
extern std::string server_identity;
extern std::string server_token;

FileBackup::FileBackup( ClientMain* client_main, int clientid, std::string clientname, std::string clientsubname, LogAction log_action, bool is_incremental, int group, bool use_tmpfiles, std::string tmpfile_path, bool use_reflink, bool use_snapshots)
	:  Backup(client_main, clientid, clientname, clientsubname, log_action, true, is_incremental), group(group), use_tmpfiles(use_tmpfiles), tmpfile_path(tmpfile_path), use_reflink(use_reflink), use_snapshots(use_snapshots),
	disk_error(false), with_hashes(false),
	backupid(-1), hashpipe(NULL), hashpipe_prepare(NULL), bsh(NULL), bsh_prepare(NULL),
	bsh_ticket(ILLEGAL_THREADPOOL_TICKET), bsh_prepare_ticket(ILLEGAL_THREADPOOL_TICKET), pingthread(NULL),
	pingthread_ticket(ILLEGAL_THREADPOOL_TICKET), cdp_path(false), metadata_download_thread_ticket(ILLEGAL_THREADPOOL_TICKET)
{
	createHashThreads(use_reflink);
}


FileBackup::~FileBackup()
{
	destroyHashThreads();
}

ServerBackupDao::SDuration FileBackup::interpolateDurations(const std::vector<ServerBackupDao::SDuration>& durations)
{
	float duration=0;
	float indexing_time_ms=0;
	if(!durations.empty())
	{
		duration = static_cast<float>(durations[durations.size()-1].duration);
		indexing_time_ms = static_cast<float>(durations[durations.size()-1].indexing_time_ms);
	}

	if(durations.size()>1)
	{
		for(size_t i=durations.size()-1;i--;)
		{
			duration = 0.9f*duration + 0.1f*durations[i].duration;
			indexing_time_ms = 0.9f*indexing_time_ms + 0.1f*durations[i].indexing_time_ms;
		}
	}

	ServerBackupDao::SDuration ret = {
		static_cast<int>(indexing_time_ms+0.5f),
		static_cast<int>(duration+0.5f) };

		return ret;
}

bool FileBackup::getResult()
{
	return backup_result;
}

bool FileBackup::request_filelist_construct(bool full, bool resume, int group,
	bool with_token, bool& no_backup_dirs, bool& connect_fail, const std::string& clientsubname)
{
	if(server_settings->getSettings()->end_to_end_file_backup_verification)
	{
		client_main->sendClientMessage("ENABLE END TO END FILE BACKUP VERIFICATION", "OK", "Enabling end to end file backup verficiation on client failed.", 10000);
	}

	unsigned int timeout_time=full_backup_construct_timeout;
	if(client_main->getProtocolVersions().file_protocol_version>=2)
	{
		timeout_time=120000;
	}

	CTCPStack tcpstack(client_main->isOnInternetConnection());

	ServerLogger::Log(logid, clientname+": Connecting for filelist...", LL_DEBUG);
	IPipe *cc=client_main->getClientCommandConnection(10000);
	if(cc==NULL)
	{
		ServerLogger::Log(logid, "Connecting to ClientService of \""+clientname+"\" failed - CONNECT error during filelist construction", LL_ERROR);
		connect_fail=true;
		return false;
	}

	std::string pver="";
	if(client_main->getProtocolVersions().file_protocol_version>=2) pver="2";
	if(client_main->getProtocolVersions().file_protocol_version_v2>=1) pver="3";

	std::string identity;
	if(!client_main->getSessionIdentity().empty())
	{
		identity=client_main->getSessionIdentity();
	}
	else
	{
		identity=server_identity;
	}

	std::string start_backup_cmd=identity+pver;

	if(full && !resume)
	{
		start_backup_cmd+="START FULL BACKUP";
	}
	else
	{
		start_backup_cmd+="START BACKUP";
	}

	if(client_main->getProtocolVersions().file_protocol_version_v2>=1)
	{
		start_backup_cmd+=" group="+convert(group);
		if(!clientsubname.empty())
		{
			start_backup_cmd+="&clientsubname="+EscapeParamString((clientsubname));
		}
	}

	if(resume && client_main->getProtocolVersions().file_protocol_version_v2>=1)
	{
		start_backup_cmd+="&resume=";
		if(full)
			start_backup_cmd+="full";
		else
			start_backup_cmd+="incr";
	}

	if(client_main->getProtocolVersions().select_sha_version>0)
	{
		start_backup_cmd+="&sha=512";
	}

	start_backup_cmd+="&with_permissions=1&with_scripts=1&with_orig_path=1&with_sequence=1&with_proper_symlinks=1";

	if(with_token)
	{
		start_backup_cmd+="#token="+server_token;
	}

	tcpstack.Send(cc, start_backup_cmd);

	ServerLogger::Log(logid, clientname+": Waiting for filelist", LL_DEBUG);
	std::string ret;
	int64 total_starttime_s = Server->getTimeSeconds();
	int64 starttime=Server->getTimeMS();
	while(Server->getTimeMS()-starttime<=timeout_time)
	{
		size_t rc=cc->Read(&ret, 60000);
		if(rc==0)
		{			
			if(client_main->getProtocolVersions().file_protocol_version<2 && Server->getTimeMS()-starttime<=20000 && with_token==true) //Compatibility with older clients
			{
				Server->destroy(cc);
				ServerLogger::Log(logid, clientname+": Trying old filelist request", LL_WARNING);
				return request_filelist_construct(full, resume, group, false, no_backup_dirs, connect_fail, clientsubname);
			}
			else
			{
				if(client_main->getProtocolVersions().file_protocol_version>=2 || pingthread->isTimeout() )
				{
					ServerLogger::Log(logid, "Constructing of filelist of \""+clientname+"\" failed - TIMEOUT(1)", LL_ERROR);
					break;
				}
				else
				{
					continue;
				}
			}
		}
		tcpstack.AddData((char*)ret.c_str(), ret.size());

		size_t packetsize;
		char *pck=tcpstack.getPacket(&packetsize);
		if(pck!=NULL && packetsize>0)
		{
			ret=pck;
			delete [] pck;
			if(ret!="DONE")
			{
				if(ret=="BUSY")
				{
					starttime=Server->getTimeMS();
				}
				else if(ret!="no backup dirs")
				{
					logVssLogdata(Server->getTimeSeconds()-total_starttime_s);
					ServerLogger::Log(logid, "Constructing of filelist of \""+clientname+"\" failed: "+ret, LL_ERROR);
					break;
				}
				else
				{
					ServerLogger::Log(logid, "Constructing of filelist of \""+clientname+"\" failed: "+ret+". Please add paths to backup on the client (via tray icon) or configure default paths to backup.", LL_ERROR);
					no_backup_dirs=true;
					break;
				}				
			}
			else
			{
				logVssLogdata(Server->getTimeSeconds()-total_starttime_s);
				Server->destroy(cc);
				return true;
			}
		}
	}
	Server->destroy(cc);
	return false;
}

bool FileBackup::hasEarlyError()
{
	return has_early_error;
}

void FileBackup::logVssLogdata(int64 vss_duration_s)
{
	std::string vsslogdata=client_main->sendClientMessage("GET VSSLOG", "Getting volume shadow copy logdata from client failed", 10000, false, LL_INFO);

	if(!vsslogdata.empty() && vsslogdata!="ERR")
	{
		std::vector<std::string> lines;
		TokenizeMail(vsslogdata, lines, "\n");
		int64 initial_time=Server->getTimeSeconds();
		for(size_t i=0;i<lines.size();++i)
		{
			size_t s1 = lines[i].find("-");
			size_t s2 = lines[i].find("-", s1+1);

			if(s1==std::string::npos)
			{
				continue;
			}

			int loglevel=atoi(lines[i].substr(0, s1).c_str());

			int64 times = 0;

			std::string msg;
			
			if(s2!=std::string::npos)
			{
				times = os_atoi64(lines[i].substr(s1+1, s2-s1));

				if(i==0)
				{
					initial_time = times;
					continue;
				}

				times = initial_time - times;

				if(times>vss_duration_s+60)
				{
					times = 0;
				}

				msg = lines[i].substr(s2+1);
			}			
			else
			{
				msg = lines[i].substr(s1+1);
			}

			std::string data=getafter("-", lines[i]);
			ServerLogger::Log(logid, msg, loglevel);
		}
	}
}

bool FileBackup::getTokenFile(FileClient &fc, bool hashed_transfer )
{
	bool has_token_file=true;
	
	IFile *tokens_file=Server->openFile(os_file_prefix(backuppath_hashes+os_file_sep()+".urbackup_tokens.properties"), MODE_WRITE);
	if(tokens_file==NULL)
	{
		ServerLogger::Log(logid, "Error opening "+backuppath_hashes+os_file_sep()+".urbackup_tokens.properties", LL_ERROR);
		return false;
	}
	_u32 rc=fc.GetFile("urbackup/tokens_"+server_token+".properties", tokens_file, hashed_transfer, false, 0);
	if(rc!=ERR_SUCCESS)
	{
		ServerLogger::Log(logid, "Error getting tokens file of "+clientname+". Errorcode: "+fc.getErrorString(rc)+" ("+convert(rc)+")", LL_DEBUG);
		has_token_file=false;
	}
	Server->destroy(tokens_file);


	std::auto_ptr<ISettingsReader> urbackup_tokens(
		Server->createFileSettingsReader(os_file_prefix(backuppath_hashes+os_file_sep()+".urbackup_tokens.properties")));

	std::string access_key;
	if(urbackup_tokens->getValue("access_key", &access_key) &&
		!access_key.empty() &&
		access_key != server_settings->getSettings()->client_access_key )
	{
		backup_dao->updateOrInsertSetting(clientid, "client_access_key", access_key);

		if(!server_settings->getSettings()->client_access_key.empty())
		{
			backup_dao->deleteUsedAccessTokens(clientid);
		}

		server_settings->update(true);
	}
	
	return has_token_file;
}

std::string FileBackup::clientlistName( int group, bool new_list )
{
	std::string ret="urbackup/clientlist_";

	if(group!=0)
	{
		ret+=convert(group)+"_";
	}

	ret+=convert(clientid);
	if(new_list)
	{
		ret+="_new";
	}
	ret+=".ub";

	return ret;
}

void FileBackup::createHashThreads(bool use_reflink)
{
	assert(bsh==NULL);
	assert(bsh_prepare==NULL);

	hashpipe=Server->createMemoryPipe();
	hashpipe_prepare=Server->createMemoryPipe();

	bsh=new BackupServerHash(hashpipe, clientid, use_snapshots, use_reflink, use_tmpfiles, logid);
	bsh_prepare=new BackupServerPrepareHash(hashpipe_prepare, hashpipe, clientid, logid);
	bsh_ticket = Server->getThreadPool()->execute(bsh);
	bsh_prepare_ticket = Server->getThreadPool()->execute(bsh_prepare);
}


void FileBackup::destroyHashThreads()
{
	hashpipe_prepare->Write("exit");
	Server->getThreadPool()->waitFor(bsh_ticket);
	Server->getThreadPool()->waitFor(bsh_prepare_ticket);

	bsh_ticket=ILLEGAL_THREADPOOL_TICKET;
	bsh_prepare_ticket=ILLEGAL_THREADPOOL_TICKET;
	hashpipe=NULL;
	hashpipe_prepare=NULL;
	bsh=NULL;
	bsh_prepare=NULL;
}

_i64 FileBackup::getIncrementalSize(IFile *f, const std::vector<size_t> &diffs, bool all)
{
	f->Seek(0);
	_i64 rsize=0;
	FileListParser list_parser;
	SFile cf;
	bool indirchange=false;
	size_t read;
	size_t line=0;
	char buffer[4096];
	int indir_currdepth=0;
	int depth=0;
	int indir_curr_depth=0;
	int changelevel=0;

	if(all)
	{
		indirchange=true;
	}

	while( (read=f->Read(buffer, 4096))>0 )
	{
		for(size_t i=0;i<read;++i)
		{
			bool b=list_parser.nextEntry(buffer[i], cf, NULL);
			if(b)
			{
				if(cf.isdir==true)
				{
					if(indirchange==false && hasChange(line, diffs) )
					{
						indirchange=true;
						changelevel=depth;
						indir_currdepth=0;
					}
					else if(indirchange==true)
					{
						if(cf.name!="..")
							++indir_currdepth;
						else
							--indir_currdepth;
					}

					if(cf.name==".." && indir_currdepth>0)
					{
						--indir_currdepth;
					}

					if(cf.name!="..")
					{
						++depth;
					}
					else
					{
						--depth;
						if(indirchange==true && depth==changelevel)
						{
							if(!all)
							{
								indirchange=false;
							}
						}
					}
				}
				else
				{
					if(indirchange==true || hasChange(line, diffs))
					{
						if(cf.size>0)
						{
							rsize+=cf.size;
						}						
					}
				}
				++line;
			}
		}

		if(read<4096)
			break;
	}

	return rsize;
}

void FileBackup::calculateEtaFileBackup( int64 &last_eta_update, int64& eta_set_time, int64 ctime, FileClient &fc, FileClientChunked* fc_chunked,
	int64 linked_bytes, int64 &last_eta_received_bytes, double &eta_estimated_speed, _i64 files_size )
{
	last_eta_update=ctime;

	int64 received_data_bytes = fc.getReceivedDataBytes() + (fc_chunked?fc_chunked->getReceivedDataBytes():0) + linked_bytes;

	int64 new_bytes =  received_data_bytes - last_eta_received_bytes;
	int64 passed_time = Server->getTimeMS() - eta_set_time;

	eta_set_time = Server->getTimeMS();

	double speed_bpms = static_cast<double>(new_bytes)/passed_time;

	if(eta_estimated_speed==0)
	{
		eta_estimated_speed = speed_bpms;
	}
	else
	{
		eta_estimated_speed = eta_estimated_speed*0.9 + eta_estimated_speed*0.1;
	}

	if(last_eta_received_bytes>0)
	{
		ServerStatus::setProcessEta(clientname, status_id,
			static_cast<int64>((files_size-received_data_bytes)/eta_estimated_speed + 0.5));
	}

	last_eta_received_bytes = received_data_bytes;
}

bool FileBackup::doBackup()
{
	if(!client_main->handle_not_enough_space(""))
	{
		return false;
	}

	if( server_settings->getSettings()->internet_mode_enabled )
	{
		if( server_settings->getSettings()->internet_incr_file_transfer_mode=="blockhash")
			with_hashes=true;
	}

	if( server_settings->getSettings()->local_incr_file_transfer_mode=="blockhash")
		with_hashes=true;

	if(!fileindex.get())
	{
		fileindex.reset(create_lmdb_files_index());
	}

	if(!cdp_path)
	{
		if(!constructBackupPath(with_hashes, use_snapshots, !r_incremental))
		{
            ServerLogger::Log(logid, "Cannot create directory "+backuppath+" for backup (server error)", LL_ERROR);
			return false;
		}
	}
	else
	{
		if(!constructBackupPathCdp())
		{
            ServerLogger::Log(logid, "Cannot create directory "+backuppath+" for backup (server error)", LL_ERROR);
			return false;
		}
	}

	pingthread =new ServerPingThread(client_main, clientname, status_id, client_main->getProtocolVersions().eta_version>0);
	pingthread_ticket=Server->getThreadPool()->execute(pingthread);

	local_hash.reset(new BackupServerHash(NULL, clientid, use_snapshots, use_reflink, use_tmpfiles, logid));
	local_hash->setupDatabase();

	

	bool backup_result = doFileBackup();

	if(pingthread!=NULL)
	{
		pingthread->setStop(true);
		Server->getThreadPool()->waitFor(pingthread_ticket);
		pingthread=NULL;
	}	

	local_hash->deinitDatabase();
	

	if(disk_error)
	{
		ServerLogger::Log(logid, "FATAL: Backup failed because of disk problems", LL_ERROR);
		client_main->sendMailToAdmins("Fatal error occured during backup", ServerLogger::getWarningLevelTextLogdata(logid));
	}

	if((!has_early_error && !backup_result) || disk_error)
	{
		sendBackupOkay(false);
	}
	else if(has_early_error)
	{
		ServerLogger::Log(logid, "Backup had an early error. Deleting partial backup.", LL_ERROR);

		deleteBackup();

	}
	else
	{
		sendBackupOkay(true);
		backup_dao->updateClientLastFileBackup(backupid, clientid);
		backup_dao->updateFileBackupSetComplete(backupid);
	}


	return backup_result;
}

bool FileBackup::hasChange(size_t line, const std::vector<size_t> &diffs)
{
	return std::binary_search(diffs.begin(), diffs.end(), line);
}

std::string FileBackup::fixFilenameForOS(const std::string& fn, std::set<std::string>& samedir_filenames, const std::string& curr_path)
{
	std::string ret;
	bool modified_filename=false;
#ifdef _WIN32
	std::string disallowed_chars = "\\:*?\"<>|/";
	for(char ch=1;ch<=31;++ch)
	{
		disallowed_chars+=ch;
	}

	if(fn=="CON" || fn=="PRN" || fn=="AUX" || fn=="NUL" || fn=="COM1" || fn=="COM2" || fn=="COM3" ||
		fn=="COM4" || fn=="COM5" || fn=="COM6" || fn=="COM7" || fn=="COM8" || fn=="COM9" || fn=="LPT1" ||
		fn=="LPT2" || fn=="LPT3" || fn=="LPT4" || fn=="LPT5" || fn=="LPT6" || fn=="LPT7" || fn=="LPT8" || fn=="LPT9")
	{
		ServerLogger::Log(logid, "Filename \""+fn+"\" not allowed on Windows. Prefixing and appending hash.", LL_WARNING);
		ret = "_" + fn;
		modified_filename=true;
	}

	if(next(fn, 0, "CON.") || next(fn, 0, "PRN.") || next(fn, 0, "AUX.") || next(fn, 0, "NUL.") || next(fn, 0, "COM1.") || next(fn, 0, "COM2.") || next(fn, 0, "COM3.") ||
		next(fn, 0, "COM4.") || next(fn, 0, "COM5.") || next(fn, 0, "COM6.") || next(fn, 0, "COM7.") || next(fn, 0, "COM8.") || next(fn, 0, "COM9.") || next(fn, 0, "LPT1.") ||
		next(fn, 0, "LPT2.") || next(fn, 0, "LPT3.") || next(fn, 0, "LPT4.") || next(fn, 0, "LPT5.") || next(fn, 0, "LPT6.") || next(fn, 0, "LPT7.") || next(fn, 0, "LPT8.") || next(fn, 0, "LPT9.") )
	{
		ServerLogger::Log(logid, "Filename \""+fn+"\" not allowed on Windows. Prefixing and appending hash.", LL_WARNING);
		ret = "_" + fn;
		modified_filename=true;
	}

	std::vector<utf8::uint16_t> tmp;
	bool unicode_err=false;
	try
	{		
		if(modified_filename)
		{
			utf8::utf8to16(ret.begin(), ret.end(), back_inserter(tmp) );
		}
		else
		{
			utf8::utf8to16(fn.begin(), fn.end(), back_inserter(tmp) );
		}
	}
	catch (...)
	{	
		unicode_err=true;
	}

	if(unicode_err)
	{
		ServerLogger::Log(logid, "Filename \""+fn+"\" has encoding problems (assuming UTF-8 encoding). Mangling filename.", LL_WARNING);

		ret.clear();
		try
		{
			utf8::utf16to8(tmp.begin(), tmp.end(), back_inserter(ret));
		}
		catch (...)
		{
		}		
		modified_filename=true;
	}


	if( (modified_filename && ret.size()>=MAX_PATH-15) || (!modified_filename && ret.size()>=MAX_PATH-15))
	{
		if(!modified_filename)
		{
			ret=fn;
		}
		ServerLogger::Log(logid, "Filename \""+ret+"\" too long. Shortening it and appending hash.", LL_WARNING);
		ret.resize(MAX_PATH-15);
		modified_filename=true;
	}

#else
	if((fn).size()>=NAME_MAX-11)
	{
		ret=fn;
		bool log_msg=true;
		do
		{
			if( log_msg )
			{
				ServerLogger::Log(logid, "Filename \""+fn+"\" too long. Shortening it.", LL_WARNING);
				log_msg=false;
			}
			ret.resize(ret.size()-1);
			modified_filename=true;
		}
		while( (ret).size()>=NAME_MAX-11 );
	}

	std::string disallowed_chars = "/";	
#endif

	for(size_t i=0;i<disallowed_chars.size();++i)
	{
		char ch = disallowed_chars[i];
		if(fn.find(ch)!=std::string::npos)
		{
			if(!modified_filename)
			{
				ret = fn;
				modified_filename=true;
			}
			ServerLogger::Log(logid, "Filename \""+fn+"\" contains '"+std::string(1, ch)+"' which the operating system does not allow in paths. Replacing '"+std::string(1, ch)+"' with '_' and appending hash.", LL_WARNING);
			ret = ReplaceChar(ret, ch, '_');
		}
	}

	if(modified_filename)
	{
		std::string hex_md5=Server->GenerateHexMD5(fn);
		ret = ret+"-"+hex_md5.substr(0, 10);
	}
	else
	{
		ret = fn;
	}

#ifdef _WIN32
	size_t idx=0;
	std::string base=ret;
	while(samedir_filenames.find(strlower(ret))!=samedir_filenames.end())
	{
		ret = base + "_" + convert(idx);
		++idx;
		modified_filename=true;
	}

	samedir_filenames.insert(strlower(ret));
#endif

	if(modified_filename)
	{
		if(curr_path.empty())
		{
			filepath_corrections[fn] = ret;
		}
		else
		{
			filepath_corrections[curr_path+"/"+fn] = ret;
		}
	}

	return ret;
}

std::string FileBackup::convertToOSPathFromFileClient(std::string path)
{
	if(os_file_sep()!="/")
	{
		for(size_t i=0;i<path.size();++i)
			if(path[i]=='/')
				path[i]=os_file_sep()[0];
	}
	return path;
}

std::string FileBackup::systemErrorInfo()
{
	std::string errmsg;
	int64 rc = os_last_error(errmsg);
	return trim(errmsg)+" (errorcode="+convert(rc)+")";
}

bool FileBackup::link_file(const std::string &fn, const std::string &short_fn, const std::string &curr_path,
	const std::string &os_path, const std::string& sha2, _i64 filesize, bool add_sql, const FileMetadata& metadata)
{
	std::string os_curr_path=convertToOSPathFromFileClient(os_path+"/"+short_fn);
	std::string os_curr_hash_path=convertToOSPathFromFileClient(os_path+"/"+escape_metadata_fn(short_fn));
	std::string dstpath=backuppath+os_curr_path;
	std::string hashpath=backuppath_hashes+os_curr_hash_path;
	std::string filepath_old;

	bool tries_once;
	std::string ff_last;
	bool hardlink_limit;
	bool copied_file;
	int64 entryid=0;
	int entryclientid = 0;
	int64 rsize = 0;
	int64 next_entryid = 0;
	bool ok=local_hash->findFileAndLink(dstpath, NULL, hashpath, sha2, filesize, std::string(), true,
		tries_once, ff_last, hardlink_limit, copied_file, entryid, entryclientid, rsize, next_entryid,
		metadata, true);

	if(ok && add_sql)
	{
		local_hash->addFileSQL(backupid, clientid, 0, dstpath, hashpath, sha2, filesize,
			(rsize>0 && rsize!=filesize)?rsize:(copied_file?filesize:0), entryid, entryclientid, next_entryid,
			copied_file);
	}

	if(ok)
	{
		ServerLogger::Log(logid, "GT: Linked file \""+fn+"\"", LL_DEBUG);
	}
	else
	{
		if(filesize!=0)
		{
			ServerLogger::Log(logid, "GT: File \""+fn+"\" not found via hash. Loading file...", LL_DEBUG);
		}
	}

	return ok;
}

void FileBackup::sendBackupOkay(bool b_okay)
{
	if(b_okay)
	{
		notifyClientBackupSuccessfull();
	}
	else
	{
		if(pingthread!=NULL)
		{
			pingthread->setStop(true);
			Server->getThreadPool()->waitFor(pingthread_ticket);
		}
		pingthread=NULL;
	}
}

void FileBackup::notifyClientBackupSuccessfull(void)
{
	client_main->sendClientMessageRetry("DID BACKUP", "OK", "Sending status (DID BACKUP) to client failed", 10000, 5);
}

void FileBackup::waitForFileThreads(void)
{
	SStatus status=ServerStatus::getStatus(clientname);
	hashpipe->Write("flush");
	hashpipe_prepare->Write("flush");
	_u32 hashqueuesize=(_u32)hashpipe->getNumElements()+(bsh->isWorking()?1:0);
	_u32 prepare_hashqueuesize=(_u32)hashpipe_prepare->getNumElements()+(bsh_prepare->isWorking()?1:0);
	while(hashqueuesize>0 || prepare_hashqueuesize>0)
	{
		ServerStatus::setProcessQueuesize(clientname, status_id, prepare_hashqueuesize, hashqueuesize);
		Server->wait(1000);
		hashqueuesize=(_u32)hashpipe->getNumElements()+(bsh->isWorking()?1:0);
		prepare_hashqueuesize=(_u32)hashpipe_prepare->getNumElements()+(bsh_prepare->isWorking()?1:0);
	}
	{
		Server->wait(10);
		while(bsh->isWorking()) Server->wait(1000);
	}	

	ServerStatus::setProcessQueuesize(clientname, status_id, 0, 0);
}

bool FileBackup::verify_file_backup(IFile *fileentries)
{
	bool verify_ok=true;

	std::ostringstream log;

	log << "Verification of file backup with id " << backupid << ". Path=" << (backuppath) << std::endl;

	unsigned int read;
	char buffer[4096];
	std::string curr_path=backuppath;
	std::string remote_path;
	size_t verified_files=0;
	SFile cf;
	fileentries->Seek(0);
	FileListParser list_parser;
	std::stack<std::set<std::string> > folder_files;
	folder_files.push(std::set<std::string>());

	while( (read=fileentries->Read(buffer, 4096))>0 )
	{
		for(size_t i=0;i<read;++i)
		{
			std::map<std::string, std::string> extras;
			bool b=list_parser.nextEntry(buffer[i], cf, &extras);
			if(b)
			{
				std::string cfn;

				if(!cf.isdir || cf.name!="..")
				{
					cfn = fixFilenameForOS(cf.name, folder_files.top(), curr_path);
				}

				if( !cf.isdir )
				{
					std::string sha256hex=(extras["sha256_verify"]);

					if(sha256hex.empty() && SHA_DEF_DIGEST_SIZE == 64)
					{
						//compatibility
						sha256hex = (extras["sha256"]);

						if(!sha256hex.empty())
						{
							for(size_t j=0;j<sha256hex.size();j+=2)
							{
								if(j+1<sha256hex.size())
								{
									std::swap(sha256hex[j], sha256hex[j+1]);
								}
							}
						}
					}

					if(sha256hex.empty())
					{
						std::string shabase64 = extras[sha_def_identifier];
						if(shabase64.empty())
						{
							std::string msg="No hash for file \""+(curr_path+os_file_sep()+cf.name)+"\" found. Verification failed.";
							verify_ok=false;
							ServerLogger::Log(logid, msg, LL_ERROR);
							log << msg << std::endl;
						}
						else if(getSHADef(curr_path+os_file_sep()+cfn)!=base64_decode_dash(shabase64))
						{
							std::string msg="Hashes for \""+(curr_path+os_file_sep()+cf.name)+"\" differ (client side hash). Verification failed.";
							verify_ok=false;
							ServerLogger::Log(logid, msg, LL_ERROR);
							log << msg << std::endl;
							save_debug_data(remote_path+"/"+cf.name,
								base64_encode_dash(getSHADef(curr_path+os_file_sep()+cfn)),
								shabase64);
						}
						else
						{
							++verified_files;
						}
					}
					else if(getSHA256(curr_path+os_file_sep()+cfn)!=sha256hex)
					{
						std::string msg="Hashes for \""+(curr_path+os_file_sep()+cf.name)+"\" differ. Verification failed.";
						verify_ok=false;
						ServerLogger::Log(logid, msg, LL_ERROR);
						log << msg << std::endl;
					}
					else
					{
						++verified_files;
					}
				}
				else
				{
					if(cf.name=="..")
					{
						curr_path=ExtractFilePath(curr_path, os_file_sep());
						remote_path=ExtractFilePath(remote_path, "/");
						
						folder_files.pop();
					}
					else
					{
						curr_path+=os_file_sep()+cfn;

						if(!remote_path.empty())
							remote_path+="/";

						remote_path+=cfn;

						folder_files.push(std::set<std::string>());
					}
				}
			}
		}
	}

	if(!verify_ok)
	{
		client_main->sendMailToAdmins("File backup verification failed", log.str());
	}
	else
	{
		ServerLogger::Log(logid, "Verified "+convert(verified_files)+" files", LL_DEBUG);
	}

	return verify_ok;
}

std::string FileBackup::getSHA256(const std::string& fn)
{
	sha256_ctx ctx;
	sha256_init(&ctx);

	IFile * f=Server->openFile(os_file_prefix(fn), MODE_READ);

	if(f==NULL)
	{
		return std::string();
	}

	char buffer[32768];
	unsigned int r;
	while( (r=f->Read(buffer, 32768))>0)
	{
		sha256_update(&ctx, reinterpret_cast<const unsigned char*>(buffer), r);
	}

	Server->destroy(f);

	unsigned char dig[32];
	sha256_final(&ctx, dig);

	return bytesToHex(dig, 32);
}

std::string FileBackup::getSHA512(const std::string& fn)
{
	sha512_ctx ctx;
	sha512_init(&ctx);

	IFile * f=Server->openFile(os_file_prefix(fn), MODE_READ);

	if(f==NULL)
	{
		return std::string();
	}

	char buffer[32768];
	unsigned int r;
	while( (r=f->Read(buffer, 32768))>0)
	{
		sha512_update(&ctx, reinterpret_cast<const unsigned char*>(buffer), r);
	}

	Server->destroy(f);

	std::string dig;
	dig.resize(64);
	sha512_final(&ctx, reinterpret_cast<unsigned char*>(&dig[0]));

	return dig;
}

std::string FileBackup::getSHADef(const std::string& fn)
{
	sha_def_ctx ctx;
	sha_def_init(&ctx);

	IFile * f=Server->openFile(os_file_prefix(fn), MODE_READ);

	if(f==NULL)
	{
		return std::string();
	}

	char buffer[32768];
	unsigned int r;
	while( (r=f->Read(buffer, 32768))>0)
	{
		sha_def_update(&ctx, reinterpret_cast<const unsigned char*>(buffer), r);
	}

	Server->destroy(f);

	std::string dig;
	dig.resize(SHA_DEF_DIGEST_SIZE);
	sha_def_final(&ctx, reinterpret_cast<unsigned char*>(&dig[0]));

	return dig;
}

bool FileBackup::hasDiskError()
{
	return disk_error;
}

bool FileBackup::constructBackupPath(bool with_hashes, bool on_snapshot, bool create_fs)
{
	if(!createDirectoryForClient())
	{
		return false;
	}

	time_t tt=time(NULL);
#ifdef _WIN32
	tm lt;
	tm *t=&lt;
	localtime_s(t, &tt);
#else
	tm *t=localtime(&tt);
#endif
	char buffer[500];
	strftime(buffer, 500, "%y%m%d-%H%M", t);
	backuppath_single=(std::string)buffer;
	std::string backupfolder=server_settings->getSettings()->backupfolder;
	backuppath=backupfolder+os_file_sep()+clientname+os_file_sep()+backuppath_single;
	backuppath_hashes=backupfolder+os_file_sep()+clientname+os_file_sep()+backuppath_single+os_file_sep()+".hashes";

	dir_pool_path = backupfolder + os_file_sep() + clientname + os_file_sep() + ".directory_pool";

	if(on_snapshot)
	{
		if(create_fs)
		{
			return SnapshotHelper::createEmptyFilesystem(clientname, backuppath_single)  && os_create_dir(os_file_prefix(backuppath_hashes));
		}
		else
		{
			return true;
		}
	}
	else
	{
		return os_create_dir(os_file_prefix(backuppath)) && os_create_dir(os_file_prefix(backuppath_hashes));	
	}
}

bool FileBackup::constructBackupPathCdp()
{
	time_t tt=time(NULL);
#ifdef _WIN32
	tm lt;
	tm *t=&lt;
	localtime_s(t, &tt);
#else
	tm *t=localtime(&tt);
#endif
	char buffer[500];
	strftime(buffer, 500, "%y%m%d-%H%M", t);
	backuppath_single="continuous_"+std::string(buffer);
	std::string backupfolder=server_settings->getSettings()->backupfolder;
	backuppath=backupfolder+os_file_sep()+clientname+os_file_sep()+backuppath_single;
	backuppath_hashes=backupfolder+os_file_sep()+clientname+os_file_sep()+backuppath_single+os_file_sep()+".hashes";

	if( os_directory_exists(os_file_prefix(backuppath)) && os_directory_exists(os_file_prefix(backuppath_hashes)))
	{
		return true;
	}

	return os_create_dir(os_file_prefix(backuppath)) && os_create_dir(os_file_prefix(backuppath_hashes));	
}

void FileBackup::createUserViews(IFile* file_list_f)
{
	std::auto_ptr<ISettingsReader> urbackup_tokens(
		Server->createFileSettingsReader(os_file_prefix(backuppath_hashes+os_file_sep()+".urbackup_tokens.properties")));

	if(urbackup_tokens.get()==NULL)
	{
		ServerLogger::Log(logid, "Cannot create user view. Token file not present.", LL_WARNING);
		return;
	}

	std::string s_real_uids = urbackup_tokens->getValue("real_uids", "");
	std::vector<std::string> uids;
	Tokenize(s_real_uids, uids, ",");

	for(size_t i=0;i<uids.size();++i)
	{
		int64 uid = os_atoi64(uids[i]);
		std::string s_gids = urbackup_tokens->getValue(uids[i]+".gids", "");
		std::vector<std::string> gids;
		Tokenize(s_gids, gids, ",");
		std::vector<int64> ids;
		ids.push_back(uid);
		for(size_t j=0;j<gids.size();++j)
		{
			ids.push_back(os_atoi64(gids[j]));
		}

		std::string accountname = base64_decode_dash(urbackup_tokens->getValue(uids[i]+".accountname", std::string()));
		accountname = greplace("/", "_", accountname);
		accountname = greplace("\\", "_", accountname);
		std::vector<size_t> identical_permission_roots = findIdenticalPermissionRoots(file_list_f, ids);
		if(!createUserView(file_list_f, ids, accountname, identical_permission_roots))
		{
			ServerLogger::Log(logid, "Error creating user view for user with id "+convert(uid), LL_WARNING);
		}
	}
}

namespace
{
	struct SDirStatItem
	{
		size_t has_perm;
		size_t id;
		size_t nodecount;
		size_t identicalcount;
	};
}

std::vector<size_t> FileBackup::findIdenticalPermissionRoots(IFile* file_list_f, const std::vector<int64>& ids)
{
	file_list_f->Seek(0);

	char buffer[4096];
	_u32 bread;
	FileListParser file_list_parser;
	std::stack<SDirStatItem> dir_permissions;
	size_t curr_id = 0;
	std::vector<size_t> identical_permission_roots;
	SFile data;
	std::stack<std::set<std::string> > folder_files;
	folder_files.push(std::set<std::string>());

	std::string curr_path;
	std::string metadata_home_path = backuppath + os_file_sep() + ".hashes";

	while((bread=file_list_f->Read(buffer, 4096))>0)
	{
		for(_u32 i=0;i<bread;++i)
		{
			std::map<std::string, std::string> extra;
			if(file_list_parser.nextEntry(buffer[i], data, &extra))
			{

				std::string osspecific_name;
				std::string permissions;

				if(!data.isdir || data.name!="..")
				{
					osspecific_name = fixFilenameForOS(data.name, folder_files.top(), curr_path);
				}

				if(data.isdir)
				{
					if(data.name=="..")
					{
						folder_files.pop();
						curr_path = ExtractFilePath(curr_path, os_file_sep());
					}
					else
					{
						folder_files.push(std::set<std::string>());
						curr_path += os_file_sep() + osspecific_name;

						std::string metadata_fn=metadata_home_path + curr_path + os_file_sep() + metadata_dir_fn;

						FileMetadata metadata;
						if(!read_metadata(metadata_fn, metadata))
						{
							ServerLogger::Log(logid, "Error reading metadata of "+curr_path, LL_WARNING);
						}
						else
						{
							permissions = metadata.file_permissions;
						}
					}
				}
				else
				{
					std::string metadata_fn=metadata_home_path + curr_path + os_file_sep() + escape_metadata_fn(osspecific_name);
					std::string filename = curr_path + os_file_sep() + osspecific_name;

					FileMetadata metadata;
					if(!read_metadata(metadata_fn, metadata))
					{
						ServerLogger::Log(logid, "Error reading metadata of "+filename, LL_WARNING);
					}
					else
					{
						permissions = metadata.file_permissions;
					}
				}

				size_t has_perm=0;
				for(size_t j=0;j<ids.size();++j)
				{
					bool denied=false;
					if(FileMetadata::hasPermission(permissions, ids[j], denied))
					{
						++has_perm;
					}
				}

				if(data.isdir)
				{
					if(data.name=="..")
					{
						SDirStatItem last_dir = {};

						if(!dir_permissions.empty())
						{
							if(dir_permissions.top().nodecount==
								dir_permissions.top().identicalcount)
							{
								identical_permission_roots.push_back(dir_permissions.top().id);
							}

							last_dir = dir_permissions.top();
						}

						dir_permissions.pop();

						if(!dir_permissions.empty())
						{
							dir_permissions.top().nodecount+=last_dir.nodecount+1;
							dir_permissions.top().identicalcount+=last_dir.identicalcount;

							if(last_dir.has_perm==dir_permissions.top().has_perm)
							{
								++dir_permissions.top().identicalcount;
							}
						}
					}
					else
					{
						SDirStatItem nsi = {
							has_perm,
							curr_id,
							0,
							0
						};

						dir_permissions.push(nsi);
					}
				}
				else
				{
					if(!dir_permissions.empty())
					{
						++dir_permissions.top().nodecount;
						if(has_perm==dir_permissions.top().has_perm)
						{
							++dir_permissions.top().identicalcount;
						}
					}
				}

				++curr_id;
			}
		}
	}

	std::sort(identical_permission_roots.begin(), identical_permission_roots.end());
	return identical_permission_roots;
}

bool FileBackup::createUserView(IFile* file_list_f, const std::vector<int64>& ids, std::string accoutname, const std::vector<size_t>& identical_permission_roots)
{
	std::string user_view_home_path = backuppath + os_file_sep() + "user_views" + os_file_sep() + accoutname;

	if(!os_create_dir_recursive(os_file_prefix(user_view_home_path)))
	{
		ServerLogger::Log(logid, "Error creating folder for user at user_views in backup storage of current backup", LL_WARNING);
		return false;
	}

	file_list_f->Seek(0);

	char buffer[4096];
	_u32 bread;
	FileListParser file_list_parser;
	std::string curr_path;
	std::string metadata_home_path = backuppath + os_file_sep() + ".hashes";
	size_t skip = 0;
	size_t id = 0;
	SFile data;
	std::stack<std::set<std::string> > folder_files;
	folder_files.push(std::set<std::string>());
	
	while((bread=file_list_f->Read(buffer, 4096))>0)
	{
		for(_u32 i=0;i<bread;++i)
		{
			std::map<std::string, std::string> extra;
			if(file_list_parser.nextEntry(buffer[i], data, &extra))
			{
				if(skip>0)
				{
					if(data.isdir)
					{
						if(data.name=="..")
						{
							--skip;

							if(skip==0)
							{
								curr_path = ExtractFilePath(curr_path, os_file_sep());
								folder_files.pop();
							}
						}
						else
						{
							++skip;
						}
					}
					++id;
					continue;
				}

				std::string osspecific_name;
				std::string permissions;

				if(!data.isdir || data.name!="..")
				{
					osspecific_name = fixFilenameForOS(data.name, folder_files.top(), curr_path);
				}

				if(data.isdir)
				{
					if(data.name=="..")
					{
						folder_files.pop();
						curr_path = ExtractFilePath(curr_path, os_file_sep());
					}
					else
					{
						folder_files.push(std::set<std::string>());
						curr_path += os_file_sep() + osspecific_name;

						std::string metadata_fn=metadata_home_path + curr_path + os_file_sep() + metadata_dir_fn;

						FileMetadata metadata;
						if(!read_metadata(metadata_fn, metadata))
						{
							ServerLogger::Log(logid, "Error reading metadata of "+curr_path, LL_WARNING);
						}

						bool has_perm = false;
						for(size_t j=0;j<ids.size();++j)
						{
							bool denied=false;
							if(FileMetadata::hasPermission(metadata.file_permissions, ids[j], denied))
							{
								if(std::binary_search(identical_permission_roots.begin(),
									identical_permission_roots.end(), id))
								{
									if(!os_link_symbolic(os_file_prefix(backuppath + curr_path),
										os_file_prefix(user_view_home_path + curr_path)))
									{
										ServerLogger::Log(logid, "Error creating symbolic link for user view (directory)", LL_WARNING);
										return false;
									}
									skip=1;
								}
								else
								{
									if(!os_create_dir(os_file_prefix(user_view_home_path + curr_path)))
									{
										ServerLogger::Log(logid, "Error creating directory for user view", LL_WARNING);
										return false;
									}
								}
								has_perm=true;
							}
						}
						
						if(!has_perm)
						{
							skip=1;
						}
					}
				}
				else
				{
					std::string metadata_fn=metadata_home_path + curr_path + os_file_sep() + escape_metadata_fn(osspecific_name);
					std::string filename = curr_path + os_file_sep() + osspecific_name;

					FileMetadata metadata;
					if(!read_metadata(metadata_fn, metadata))
					{
						ServerLogger::Log(logid, "Error reading metadata of "+filename, LL_WARNING);
					}

					for(size_t j=0;j<ids.size();++j)
					{
						bool denied=false;
						if(FileMetadata::hasPermission(permissions, ids[j], denied))
						{
							if(!os_link_symbolic(os_file_prefix(backuppath + filename),
								os_file_prefix(user_view_home_path + filename)))
							{
								ServerLogger::Log(logid, "Error creating symbolic link for user view (file)", LL_WARNING);
								return false;
							}
							break;
						}
					}
				}

				++id;
			}
		}
	}

	std::string backupfolder = server_settings->getSettings()->backupfolder;
	std::string o_user_view_folder = backupfolder+os_file_sep()+"user_views" + os_file_sep()+clientname+ os_file_sep()+(accoutname);

	if(!os_directory_exists(os_file_prefix(o_user_view_folder)) &&
		!os_create_dir_recursive(o_user_view_folder))
	{
		ServerLogger::Log(logid, "Error creating folder for user at user_views in backup storage", LL_WARNING);
		return false;
	}

	if(!os_link_symbolic(os_file_prefix(user_view_home_path),
		os_file_prefix(o_user_view_folder + os_file_sep() + backuppath_single)))
	{
		ServerLogger::Log(logid, "Error creating user view link at user_views in backup storage", LL_WARNING);
		return false;
	}

	os_remove_symlink_dir(os_file_prefix(o_user_view_folder + os_file_sep() + "current"));
	if(!os_link_symbolic(os_file_prefix(user_view_home_path),
		os_file_prefix(o_user_view_folder + os_file_sep() + "current")))
	{
		ServerLogger::Log(logid, "Error creating current user view link at user_views in backup storage", LL_WARNING);
		return false;
	}

	return true;
}

void FileBackup::saveUsersOnClient()
{
	std::auto_ptr<ISettingsReader> urbackup_tokens(
		Server->createFileSettingsReader(os_file_prefix(backuppath_hashes+os_file_sep()+".urbackup_tokens.properties")));

	if(urbackup_tokens.get()==NULL)
	{
		ServerLogger::Log(logid, "Cannot determine users on client. Token file not present.", LL_WARNING);
		return;
	}

	std::string s_uids = urbackup_tokens->getValue("uids", "");
	std::vector<std::string> uids;
	Tokenize(s_uids, uids, ",");

	backup_dao->deleteAllUsersOnClient(clientid);

	for(size_t i=0;i<uids.size();++i)
	{
		std::string accountname = (base64_decode_dash(urbackup_tokens->getValue(uids[i]+".accountname", std::string())));
		backup_dao->addUserOnClient(clientid, accountname);

		backup_dao->addUserToken(accountname, urbackup_tokens->getValue(uids[i]+".token", std::string()));

		std::string s_gids = urbackup_tokens->getValue(uids[i]+".gids", "");
		std::vector<std::string> gids;
		Tokenize(s_gids, gids, ",");

		for(size_t j=0;j<gids.size();++j)
		{
			backup_dao->addUserToken(accountname, urbackup_tokens->getValue(gids[j]+".token", std::string()));
		}
	}

	std::vector<std::string> keys = urbackup_tokens->getKeys();
	for(size_t i=0;i<keys.size();++i)
	{
		if(keys[i].find(".token")==keys[i].size()-6)
		{
			backup_dao->addClientToken(clientid, urbackup_tokens->getValue(keys[i], std::string()));
		}
	}
}

void FileBackup::deleteBackup()
{
	if(backupid==-1)
	{
		if(use_snapshots)
		{
			if(!SnapshotHelper::removeFilesystem(clientname, backuppath_single) )
			{
				remove_directory_link_dir(backuppath, *backup_dao, clientid);
			}
		}
		else
		{
			remove_directory_link_dir(backuppath, *backup_dao, clientid);
		}	
	}
	else
	{				
		Server->getThreadPool()->executeWait(new ServerCleanupThread(CleanupAction(server_settings->getSettings()->backupfolder, clientid, backupid, true) ) );
	}
}

bool FileBackup::createSymlink(const std::string& name, size_t depth, const std::string& symlink_target, const std::string& dir_sep, bool isdir )
{
	std::vector<std::string> toks;
	TokenizeMail(symlink_target, toks, dir_sep);

	std::string target;

	for(size_t i=0;i<depth;++i)
	{
		target+=".."+os_file_sep();
	}

	for(size_t i=0;i<toks.size();++i)
	{
		std::set<std::string> emptyset;
		std::string emptypath;
		std::string component = fixFilenameForOS(toks[i], emptyset, emptypath);

		if(component==".." || component==".")
			continue;

		target+=component;

		if(i+1<toks.size())
		{
			target+=os_file_sep();
		}
	}

	return os_link_symbolic(target, name, NULL, &isdir);
}

bool FileBackup::startFileMetadataDownloadThread()
{

	if(client_main->getProtocolVersions().file_meta>0)
	{
		std::string identity = client_main->getSessionIdentity().empty()?server_identity:client_main->getSessionIdentity();
		std::auto_ptr<FileClient> fc_metadata_stream(new FileClient(false, identity, client_main->getProtocolVersions().filesrv_protocol_version,
			client_main->isOnInternetConnection(), client_main, use_tmpfiles?NULL:client_main));

		_u32 rc=client_main->getClientFilesrvConnection(fc_metadata_stream.get(), server_settings.get(), 10000);
		if(rc!=ERR_CONNECTED)
		{
			ServerLogger::Log(logid, "Full Backup of "+clientname+" failed - CONNECT error (for metadata stream)", LL_ERROR);
			has_early_error=true;
			log_backup=false;
			return false;
		}

        metadata_download_thread.reset(new server::FileMetadataDownloadThread(fc_metadata_stream.release(), server_token, logid));

		metadata_download_thread_ticket = Server->getThreadPool()->execute(metadata_download_thread.get());
	}	

	return true;
}

bool FileBackup::stopFileMetadataDownloadThread()
{
	if(metadata_download_thread.get()!=NULL)
	{
		if(!Server->getThreadPool()->waitFor(metadata_download_thread_ticket, 1000))
		{
			ServerLogger::Log(logid, "Waiting for metadata download stream to finish", LL_INFO);

			do
			{
				std::string identity = client_main->getSessionIdentity().empty()?server_identity:client_main->getSessionIdentity();
				std::auto_ptr<FileClient> fc_metadata_stream_end(new FileClient(false, identity, client_main->getProtocolVersions().filesrv_protocol_version,
					client_main->isOnInternetConnection(), client_main, use_tmpfiles?NULL:client_main));

				_u32 rc=client_main->getClientFilesrvConnection(fc_metadata_stream_end.get(), server_settings.get(), 10000);
				if(rc==ERR_CONNECTED)
				{
					fc_metadata_stream_end->InformMetadataStreamEnd(server_token);
				}

				ServerLogger::Log(logid, "Waiting for metadata download stream to finish", LL_DEBUG);
				Server->wait(1000);

				metadata_download_thread->shutdown();
			}
			while(!Server->getThreadPool()->waitFor(metadata_download_thread_ticket, 10000));
		}	

		if(!disk_error && !has_early_error && !metadata_download_thread->getHasError())
		{
			return metadata_download_thread->applyMetadata(backuppath_hashes, backuppath, client_main, filepath_corrections);
		}
	}

	return true;
}

void FileBackup::save_debug_data(const std::string& rfn, const std::string& local_hash, const std::string& remote_hash)
{
	ServerLogger::Log(logid, "Local hash: "+local_hash+" remote hash: "+remote_hash, LL_INFO);
	ServerLogger::Log(logid, "Trying to download "+rfn, LL_INFO);

	std::string identity = client_main->getSessionIdentity().empty()?server_identity:client_main->getSessionIdentity();
	FileClient fc(false, identity, client_main->getProtocolVersions().filesrv_protocol_version,
		client_main->isOnInternetConnection(), client_main, use_tmpfiles?NULL:client_main);

	_u32 rc=client_main->getClientFilesrvConnection(&fc, server_settings.get(), 10000);
	if(rc!=ERR_CONNECTED)
	{
		ServerLogger::Log(logid, "Cannot connect to retrieve file that failed to verify - CONNECT error", LL_ERROR);
		return;
	}

	std::auto_ptr<IFile> tmpfile(Server->openTemporaryFile());
	std::string tmpdirname = tmpfile->getFilename();
	tmpfile.reset();
	Server->deleteFile(tmpdirname);
	os_create_dir(tmpdirname);

	std::auto_ptr<IFile> output_file(Server->openFile(tmpdirname+os_file_sep()+"verify_failed.file", MODE_WRITE));
	rc = fc.GetFile((rfn), output_file.get(), true, false, 0);

	if(rc!=ERR_SUCCESS)
	{
		ServerLogger::Log(logid, "Error downloading "+rfn+" after verification failed", LL_ERROR);
	}
	else
	{
		output_file.reset();
		std::string sha512 = base64_encode_dash(getSHA512(tmpdirname+os_file_sep()+"verify_failed.file"));
		std::string sha256 = getSHA256(tmpdirname+os_file_sep()+"verify_failed.file");
		ServerLogger::Log(logid, "Downloaded file "+rfn+" with failed verification to "+tmpdirname+" for analysis. "
			" SHA512: "+sha512+" SHA256: "+sha256, LL_INFO);
	}
}