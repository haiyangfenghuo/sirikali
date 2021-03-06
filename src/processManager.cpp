/*
 *
 *  Copyright (c) 2020
 *  name : Francis Banyikwa
 *  email: mhogomchungu@gmail.com
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "processManager.h"

static const char * _backEndTimedOut       = "BackendTimedOut" ;
static const char * _backEndFailedToFinish = "BackendFailedToFinish" ;

struct result
{
	engines::engine::error type ;
	QByteArray outPut ;
} ;

enum class encode{ True,False } ;
static QString _make_path( QString e,encode s )
{
	if( e.startsWith( "\"" ) ){

		e.remove( 0,1 ) ;
	}

	if( e.endsWith( "\"" ) ){

		e.truncate( e.size() -1 ) ;
	}

	if( s == encode::True ){

		return engines::engine::encodeMountPath( e ) ;
	}else{
		return e ;
	}
}

template< typename Function >
static result _read( QProcess& exe,Function function )
{
	QByteArray m ;
	QByteArray s ;

	int counter = 1 ;
	int notRunningCounter = 0 ;
	int maxNotRunningCounter = 2 ;

	engines::engine::error r ;

	while( true ){

		if( exe.state() == QProcess::NotRunning ){

			utility::debug() << "warning, a process is no longer running" ;

			if( notRunningCounter < maxNotRunningCounter ){

				notRunningCounter++ ;
			}else{
				m = m + "\n-------------\n\nSiriKali::Error Backend Disappeared Unexpectedly." ;

				r = engines::engine::error::Failed ;

				break ;
			}
		}

		s = exe.readAllStandardOutput() ;

		if( !s.isEmpty() ){

			m += s ;
		}

		if( m.contains( "cygfuse: " ) || m.contains( "Cannot create WinFsp-FUSE file system:" ) ){

			r = engines::engine::error::Failed ;

			break ;
		}else{
			r = function( m ) ;

			if( r == engines::engine::error::Continue ){

				utility::Task::suspendForOneSecond() ;
				counter++ ;
			}else{
				break ;
			}
		}
	}

	utility::debug() << "Backend took " + QString::number( counter ) + " seconds to unlock a volume" ;

	return { r,std::move( m ) } ;
}

static result _getProcessOutput( QProcess& exe,const engines::engine& engine )
{
	int timeOut = engine.backendTimeout() ;

	if( timeOut > 0 ){

		int counter = 0 ;

		return _read( exe,[ & ]( const QString& e ){

			if( counter < timeOut ){

				counter++ ;
				return engine.errorCode( e ) ;
			}else{
				return engines::engine::error::Timeout ;
			}
		} ) ;
	}else{
		return _read( exe,[ & ]( const QString& e ){

			return engine.errorCode( e ) ;
		} ) ;
	}
}

struct terminate_process{

	QProcess& exe ;
	const QProcessEnvironment& env ;
	const QString& mountPath ;
	const QStringList& unMountCommand ;
};

struct terminate_result{

	Task::process::result result ;
	QString exe ;
	QStringList args ;
} ;

static terminate_result _failed_to_finish( QString exe,QStringList args )
{
	auto a = Task::process::result( _backEndFailedToFinish,
					QByteArray(),
					-1,
					0,
					true ) ;

	return { std::move( a ),std::move( exe ),std::move( args ) } ;
}

static terminate_result _terminate_process( const terminate_process& e )
{
	if( e.unMountCommand.isEmpty() ){

		auto a = "SiriKali Error: Unmount Command Not Set" ;
		auto b = Task::process::result( a,QByteArray(),-1,0,true ) ;

		return { std::move( b ),{},{} } ;
	}

	auto args = e.unMountCommand ;

	QString exe = args.takeAt( 0 ) ;

	if( exe == "SIGTERM" ){

		utility::debug() << "Terminating a process by sending it SIGTERM" ;

		e.exe.terminate() ;

		if( utility::waitForFinished( e.exe ) ){

			auto a = Task::process::result( QByteArray(),
							QByteArray(),
							0,
							0,
							true ) ;

			return { std::move( a ),std::move( exe ),std::move( args ) } ;
		}else{
			return _failed_to_finish( std::move( exe ),std::move( args ) ) ;
		}
	}

	for( auto& it : args ){

		if( it == "%{PID}" ){

			it = QString::number( e.exe.processId() ) ;

		}else if( it == "%{mountPoint}" ){

			it = e.mountPath ;
		}
	}

	utility::logger logger ;

	logger.showText( exe,args ) ;

	auto m = utility::unwrap( Task::process::run( exe,args,-1,"",e.env ) ) ;

	logger.showText( m ) ;

	if( m.success() ){

		if( utility::waitForFinished( e.exe ) ){

			return { std::move( m ),std::move( exe ),std::move( args ) } ;
		}else{
			return _failed_to_finish( std::move( exe ),std::move( args ) ) ;
		}
	}else{
		return { std::move( m ),std::move( exe ),std::move( args ) } ;
	}
}

Task::process::result processManager::run( const processManager::opts& s )
{
	if( s.create ){

		if( s.engine.autoMountsOnCreate() ){

			return this->add( s ) ;
		}else{
			utility::logger logger ;

			logger.showText( s.args.cmd,s.args.cmd_args ) ;

			auto m = utility::unwrap( Task::process::run( s.args.cmd,s.args.cmd_args,-1,s.password ) ) ;

			logger.showText( m ) ;

			return m ;
		}
	}else{
		return this->add( s ) ;
	}
}

bool processManager::backEndTimedOut( const QString& e)
{
	return e == _backEndTimedOut ;
}

Task::process::result processManager::add( const processManager::opts& opts )
{
	auto exe = utility2::unique_qptr< QProcess >() ;

	exe->setProcessEnvironment( opts.engine.getProcessEnvironment() ) ;
	exe->setProcessChannelMode( QProcess::MergedChannels ) ;
	exe->start( opts.args.cmd,opts.args.cmd_args ) ;
	exe->waitForStarted() ;
	exe->write( opts.password + "\n" ) ;
	exe->closeWriteChannel() ;

	utility::logger logger ;

	logger.showText( opts.args.cmd,opts.args.cmd_args ) ;

	auto m = _getProcessOutput( *exe,opts.engine ) ;

	auto error = []( const QByteArray& e ){

		return Task::process::result( e,QByteArray(),-1,0,true ) ;
	} ;

	auto s = [ & ](){

		if( m.type == engines::engine::error::Timeout ){

			const auto& ee = opts.engine.unMountCommand() ;
			const auto& ss = opts.args.mountPath ;

			_terminate_process( { *exe,opts.engine.getProcessEnvironment(),ss,ee } ) ;

			return error( _backEndTimedOut ) ;

		}else if( m.type == engines::engine::error::Success ){

			if( exe->state() == QProcess::Running ){

				exe->closeReadChannel( QProcess::StandardError ) ;
				exe->closeReadChannel( QProcess::StandardOutput ) ;

				this->addEntry( opts,std::move( exe ) ) ;

				m_updateVolumeList() ;

				return Task::process::result( 0 ) ;
			}else{
				utility::waitForFinished( *exe ) ;

				auto a = "SiriKali::Error: Backend Reported \"Success\" But Its No Longer Running\nGenerated Logs Are:\n" ;

				QString b = "std error\n----------------------\n" + exe->readAllStandardError() + "\n----------------------\n" ;

				QString c = "std out\n----------------------\n" + exe->readAllStandardOutput() + "\n----------------------\n" ;

				return error( QString( "%1%2%3" ).arg( a,b,c ).toLatin1() ) ;
			}
		}else{
			utility::waitForFinished( *exe ) ;

			return error( m.outPut ) ;
		}
	}() ;

	logger.showText( s ) ;

	return s ;
}

Task::process::result processManager::remove( const QStringList& unMountCommand,
					      const QString& mountPoint)
{
	Task::process::result r ;

	this->ProcessEntriesAndRemove( [ & ]( const processManager::Process& s )->result{

		if( s.mountPoint() == mountPoint ){

			auto m = _terminate_process( { s.exe(),s.env(),s.mountPoint(),unMountCommand } ) ;

			if( m.result.success() ) {

				m_updateVolumeList() ;

				r = Task::process::result( 0 ) ;

				return { true,true } ;
			}else{
				r = Task::process::result( "","Failed To Terminate A Process",1,0,true ) ;

				return { true,false } ;
			}
		}else{
			return { false,false } ;
		}
	} ) ;

	return r ;
}

std::vector<QStringList> processManager::commands() const
{
	std::vector< QStringList > s ;

	this->ProcessEntries( [ & ]( const processManager::Process& p ){

		auto e = p.exe().arguments() ;

		for( auto& m : e ){

			m = engines::engine::encodeMountPath( m ) ;
		}

		s.emplace_back( std::move( e ) ) ;

		return false ;
	} ) ;

	return s ;
}

std::vector< engines::engine::Wrapper > processManager::enginesList() const
{
	std::vector< engines::engine::Wrapper > m ;

	this->ProcessEntries( [ & ]( const processManager::Process& p ){

		m.emplace_back( p.engine() ) ;

		return false ;
	} ) ;

	return m ;
}

QString processManager::volumeProperties( const QString& mm ) const
{
	QString m ;

	this->ProcessEntries( [ & ]( const processManager::Process& e ){

		if( mm == e.mountPoint() ){

			m = QObject::tr( "Mount Options:\n\n" ) ;

			for( const auto& it : e.arguments() ){

				if( it != "-o" ){

					m += it + "\n" ;
				}
			}

			return true ;
		}else{
			return false ;
		}
	} ) ;

	return m ;
}

void processManager::updateVolumeList( std::function< void() > function )
{
	m_updateVolumeList = std::move( function ) ;
}

std::vector< processManager::mountOpts > processManager::mountOptions() const
{
	std::vector< processManager::mountOpts > mOpts ;

	this->ProcessEntries( [ & ]( const processManager::Process& p ){

		const auto& m = p.args() ;

		auto a = _make_path( m.cipherPath,encode::True ) ;
		auto b = _make_path( m.mountPath,encode::True ) ;

		mOpts.emplace_back( m.mode,m.subtype,a,b,m.fuseOptions,p.engine() ) ;

		return false ;
	} ) ;

	return mOpts ;
}

bool processManager::mountPointTaken( const QString& ee ) const
{
	bool m = false ;

	auto e = "\"" + QDir::toNativeSeparators( ee ) + "\"" ;

	this->ProcessEntries( [ & ]( const processManager::Process& p ){

		if( QDir::toNativeSeparators( p.mountPoint() ) == e ){

			m = true ;
			return true ;
		}else{
			return false ;
		}
	} ) ;

	return m ;
}

void processManager::removeInActive()
{
	auto _remove_inactive = [ & ]{

		bool entryRemoved = false ;

		this->ProcessEntriesAndRemove( [ & ]( const processManager::Process& p )->result{

			if( p.notRunning() ){

				entryRemoved = true ;

				return { true,true } ;
			}else{
				return { false,false } ;
			}
		} ) ;

		return entryRemoved ;
	} ;

	while( _remove_inactive() ){}
}
