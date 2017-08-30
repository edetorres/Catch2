/*
 *  Created by Phil on 31/10/2010.
 *  Copyright 2010 Two Blue Cubes Ltd. All rights reserved.
 *
 *  Distributed under the Boost Software License, Version 1.0. (See accompanying
 *  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 */
#ifndef TWOBLUECUBES_CATCH_RUNNER_HPP_INCLUDED
#define TWOBLUECUBES_CATCH_RUNNER_HPP_INCLUDED

#include "internal/catch_commandline.hpp"
#include "internal/catch_console_colour.hpp"
#include "internal/catch_enforce.h"
#include "internal/catch_list.h"
#include "internal/catch_run_context.hpp"
#include "internal/catch_stream.h"
#include "internal/catch_test_spec.hpp"
#include "internal/catch_version.h"
#include "internal/catch_interfaces_reporter.h"
#include "internal/catch_random_number_generator.h"
#include "internal/catch_startup_exception_registry.h"
#include "internal/catch_text.h"

#include <fstream>
#include <cstdlib>
#include <limits>
#include <iomanip>

namespace Catch {

    IStreamingReporterPtr createReporter( std::string const& reporterName, IConfigPtr const& config ) {
        auto reporter = getRegistryHub().getReporterRegistry().create( reporterName, config );
        CATCH_ENFORCE( reporter, "No reporter registered with name: '" << reporterName << "'" );

        return reporter;
    }

#ifndef CATCH_CONFIG_DEFAULT_REPORTER
#define CATCH_CONFIG_DEFAULT_REPORTER "console"
#endif

    IStreamingReporterPtr makeReporter( std::shared_ptr<Config> const& config ) {
        auto const& reporterNames = config->getReporterNames();
        if( reporterNames.empty() )
            return createReporter(CATCH_CONFIG_DEFAULT_REPORTER, config );

        IStreamingReporterPtr reporter;
        for( auto const& name : reporterNames )
            addReporter( reporter, createReporter( name, config ) );
        return reporter;
    }
    void addListeners( IStreamingReporterPtr& reporters, IConfigPtr const& config ) {
        auto const& listeners = getRegistryHub().getReporterRegistry().getListeners();
        for( auto const& listener : listeners )
            addReporter(reporters, listener->create( ReporterConfig( config ) ) );
    }


    Totals runTests( std::shared_ptr<Config> const& config ) {

        IStreamingReporterPtr reporter = makeReporter( config );
        addListeners( reporter, config );

        RunContext context( config, std::move( reporter ) );

        Totals totals;

        context.testGroupStarting( config->name(), 1, 1 );

        TestSpec testSpec = config->testSpec();
        if( !testSpec.hasFilters() )
            testSpec = TestSpecParser( ITagAliasRegistry::get() ).parse( "~[.]" ).testSpec(); // All not hidden tests

        std::vector<TestCase> const& allTestCases = getAllTestCasesSorted( *config );
        for( auto const& testCase : allTestCases ) {
            if( !context.aborting() && matchTest( testCase, testSpec, *config ) )
                totals += context.runTest( testCase );
            else
                context.reporter().skipTest( testCase );
        }

        context.testGroupEnded( config->name(), totals, 1, 1 );
        return totals;
    }

    void applyFilenamesAsTags( IConfig const& config ) {
        auto& tests = const_cast<std::vector<TestCase>&>( getAllTestCasesSorted( config ) );
        for( auto& testCase : tests ) {
            auto tags = testCase.tags;

            std::string filename = testCase.lineInfo.file;
            std::string::size_type lastSlash = filename.find_last_of( "\\/" );
            if( lastSlash != std::string::npos )
                filename = filename.substr( lastSlash+1 );

            std::string::size_type lastDot = filename.find_last_of( '.' );
            if( lastDot != std::string::npos )
                filename = filename.substr( 0, lastDot );

            tags.push_back( '#' + filename );
            setTags( testCase, tags );
        }
    }


    class Session : NonCopyable {
        static const int MaxExitCode;
    public:

        Session() {
            static bool alreadyInstantiated = false;
            if( alreadyInstantiated )
                CATCH_INTERNAL_ERROR( "Only one instance of Catch::Session can ever be used" );
            alreadyInstantiated = true;
            m_cli = makeCommandLineParser( m_configData );
        }
        ~Session() override {
            Catch::cleanUp();
        }

        void showHelp() const {
            Catch::cout()
                    << "\nCatch v" << libraryVersion() << "\n"
                    << m_cli << std::endl
                    << "For more detailed usage please see the project docs\n" << std::endl;
        }
        void libIdentify() {
            Catch::cout()
                    << std::left << std::setw(16) << "description: " << "A Catch test executable\n"
                    << std::left << std::setw(16) << "category: " << "testframework\n"
                    << std::left << std::setw(16) << "framework: " << "Catch Test\n"
                    << std::left << std::setw(16) << "version: " << libraryVersion() << std::endl;
        }

        int applyCommandLine( int argc, char* argv[] ) {
            auto result = m_cli.parse( clara::Args( argc, argv ) );
            if( !result ) {
                Catch::cerr()
                    << Colour( Colour::Red )
                    << "\nError(s) in input:\n"
                    << Column( result.errorMessage() ).indent( 2 )
                    << "\n\n";
                Catch::cerr() << "Run with -? for usage\n" << std::endl;
                return MaxExitCode;
            }

            if( m_configData.showHelp )
                showHelp();
            if( m_configData.libIdentify )
                libIdentify();
            m_config.reset();
            return 0;
        }

        void useConfigData( ConfigData const& configData ) {
            m_configData = configData;
            m_config.reset();
        }

        int run( int argc, char* argv[] ) {
            const auto& exceptions = getRegistryHub().getStartupExceptionRegistry().getExceptions();
            if ( !exceptions.empty() ) {
                Catch::cerr() << "Errors occured during startup!" << '\n';
                // iterate over all exceptions and notify user
                for ( const auto& ex_ptr : exceptions ) {
                    try {
                        std::rethrow_exception(ex_ptr);
                    } catch ( std::exception const& ex ) {
                        Catch::cerr() << ex.what() << '\n';
                    }
                }
                return 1;
            }
            int returnCode = applyCommandLine( argc, argv );
            if( returnCode == 0 )
                returnCode = run();
            return returnCode;
        }

    #if defined(WIN32) && defined(UNICODE)
        int run( int argc, wchar_t* const argv[] ) {

            char **utf8Argv = new char *[ argc ];

            for ( int i = 0; i < argc; ++i ) {
                int bufSize = WideCharToMultiByte( CP_UTF8, 0, argv[i], -1, NULL, 0, NULL, NULL );

                utf8Argv[ i ] = new char[ bufSize ];

                WideCharToMultiByte( CP_UTF8, 0, argv[i], -1, utf8Argv[i], bufSize, NULL, NULL );
            }

            int returnCode = run( argc, utf8Argv );

            for ( int i = 0; i < argc; ++i )
                delete [] utf8Argv[ i ];

            delete [] utf8Argv;

            return returnCode;
        }
    #endif
        int run() {
            if( ( m_configData.waitForKeypress & WaitForKeypress::BeforeStart ) != 0 ) {
                Catch::cout() << "...waiting for enter/ return before starting" << std::endl;
                static_cast<void>(std::getchar());
            }
            int exitCode = runInternal();
            if( ( m_configData.waitForKeypress & WaitForKeypress::BeforeExit ) != 0 ) {
                Catch::cout() << "...waiting for enter/ return before exiting, with code: " << exitCode << std::endl;
                static_cast<void>(std::getchar());
            }
            return exitCode;
        }

        clara::Parser const& cli() const {
            return m_cli;
        }
        void cli( clara::Parser const& newParser ) {
            m_cli = newParser;
        }
        ConfigData& configData() {
            return m_configData;
        }
        Config& config() {
            if( !m_config )
                m_config = std::make_shared<Config>( m_configData );
            return *m_config;
        }
    private:
        int runInternal() {
            if( m_configData.showHelp || m_configData.libIdentify )
                return 0;

            try
            {
                config(); // Force config to be constructed

                seedRng( *m_config );

                if( m_configData.filenamesAsTags )
                    applyFilenamesAsTags( *m_config );

                // Handle list request
                if( Option<std::size_t> listed = list( config() ) )
                    return static_cast<int>( *listed );

                return (std::min)( MaxExitCode, static_cast<int>( runTests( m_config ).assertions.failed ) );
            }
            catch( std::exception& ex ) {
                Catch::cerr() << ex.what() << std::endl;
                return MaxExitCode;
            }
        }

        clara::Parser m_cli;
        ConfigData m_configData;
        std::shared_ptr<Config> m_config;
    };

    const int Session::MaxExitCode = 255;

} // end namespace Catch

#endif // TWOBLUECUBES_CATCH_RUNNER_HPP_INCLUDED
