#include "minorGems/network/SocketClient.h"
#include "minorGems/system/Thread.h"
#include "minorGems/util/random/JenkinsRandomSource.h"
#include "minorGems/formats/encodingUtils.h"

JenkinsRandomSource randSource;


void usage() {
    printf( "Usage:\n" );
    printf( "stressTestClient server_address server_port email_prefix num_clients\n\n" );
    
    printf( "Example:\n" );
    printf( "stressTestClient onehouronelife.com 8005 dummy 100\n\n" );
    
    exit( 1 );
}


typedef struct Client {
        int i;
        
        Socket *sock;
        SimpleVector<unsigned char> buffer;
        
        int skipCompressedData;
        
        char pendingCMData;
        int pendingCMCompressedSize;
        int pendingCMDecompressedSize;

        int id;
        int x, y;

        char moving;
        char dead;
        char disconnected;
        
} Client;


// NULL if no message read
char *getNextMessage( Client *inC ) {

    if( inC->skipCompressedData > 0 && inC->buffer.size() > 0 ) {
        int numToDelete = inC->skipCompressedData;
        
        if( numToDelete > inC->buffer.size() ) {
            numToDelete = inC->buffer.size();
        }
        
        inC->buffer.deleteStartElements( numToDelete );
        inC->skipCompressedData -= numToDelete;
    }
    
    if( inC->skipCompressedData > 0 ) {
        printf( "Client %d still needs to skip %d compressed bytes\n",
                inC->i, inC->skipCompressedData );
        Thread::staticSleep( 1000 );
        
        unsigned char buffer[512];
    
        int nextReadSize = inC->skipCompressedData;
        
        if( nextReadSize > 512 ) {
            nextReadSize = 512;
        }
        int numRead = nextReadSize;
        
        while( numRead == nextReadSize && nextReadSize > 0 ) {
            nextReadSize = inC->skipCompressedData;
        
            if( nextReadSize > 512 ) {
                nextReadSize = 512;
            }

            numRead = inC->sock->receive( buffer, nextReadSize, 0 );
            
            if( numRead == -1 ) {
                inC->disconnected = true;
            }

            if( numRead > 0 ) {
                inC->skipCompressedData -= numRead;
            }
        }

        if( numRead != nextReadSize && nextReadSize > 0 ) {
            // timed out waiting for rest of compressed data
            return NULL;
        }
    }
    
    // read all available data
    unsigned char buffer[512];
    
    int numRead = inC->sock->receive( buffer, 512, 0 );

    if( numRead == -1 ) {
        inC->disconnected = true;
    }
    
    
    while( numRead > 0 ) {
        inC->buffer.appendArray( buffer, numRead );
        numRead = inC->sock->receive( buffer, 512, 0 );
        
        if( numRead == -1 ) {
            inC->disconnected = true;
        }
    }

    if( inC->pendingCMData ) {
        if( inC->buffer.size() >= inC->pendingCMCompressedSize ) {
            inC->pendingCMData = false;
            
            unsigned char *compressedData = 
                new unsigned char[ inC->pendingCMCompressedSize ];
            
            for( int i=0; i<inC->pendingCMCompressedSize; i++ ) {
                compressedData[i] = inC->buffer.getElementDirect( i );
            }
            inC->buffer.deleteStartElements( inC->pendingCMCompressedSize );
            
            unsigned char *decompressedMessage =
                zipDecompress( compressedData, 
                               inC->pendingCMCompressedSize,
                               inC->pendingCMDecompressedSize );

            delete [] compressedData;

            if( decompressedMessage == NULL ) {
                printf( "Decompressing CM message failed\n" );
                return NULL;
            }
            else {
                char *textMessage = 
                    new char[ inC->pendingCMDecompressedSize + 1 ];
                memcpy( textMessage, decompressedMessage,
                       inC->pendingCMDecompressedSize );
                textMessage[ inC->pendingCMDecompressedSize ] = '\0';
                
                delete [] decompressedMessage;
                
                return textMessage;
            }
        }
        else {
            // wait for more data to arrive
            return NULL;
        }
    }

    // find first terminal character #
    int index = inC->buffer.getElementIndex( '#' );
        
    if( index == -1 ) {
        return NULL;
    }

    char *message = new char[ index + 1 ];
    
    // all but terminal character
    for( int i=0; i<index; i++ ) {
        message[i] = (char)( inC->buffer.getElementDirect( i ) );
    }
    
    // delete from buffer, including terminal character
    inC->buffer.deleteStartElements( index + 1 );

    message[ index ] = '\0';
    
    if( strstr( message, "CM" ) == message ) {
        inC->pendingCMData = true;
        
        sscanf( message, "CM\n%d %d\n", 
                &( inC->pendingCMDecompressedSize ), 
                &( inC->pendingCMCompressedSize ) );


        delete [] message;
        
        return NULL;
    }

    return message;
}


void parsePlayerUpdateMessage( Client *inC, char *inMessageLine ) {
    SimpleVector<char*> *tokens = tokenizeString( inMessageLine );

    //printf( "\n\nParsing PU line: %s\n\n", inMessageLine );
    
    if( tokens->size() > 16 ) {
        int id = -1;
        sscanf( tokens->getElementDirect(0), "%d", &( id ) );
    
        if( inC->id == -1 ) {
            inC->id = id;
        }
        
        if( inC->id == id ) {
            // update pos
            
            if( inC->moving ) {
                //printf( "Client %d done moving\n", inC->i );
            }
            if( strcmp( tokens->getElementDirect(14), "X" ) == 0 ) {
                // dead
                inC->dead = true;
                printf( "Client %d died with PU message:  %s\n",
                        inC->i, inMessageLine );
            }
            else {
                sscanf( tokens->getElementDirect(14), "%d", &( inC->x ) );
                sscanf( tokens->getElementDirect(15), "%d", &( inC->y ) );
                inC->moving = false;
            }
        }
    }
    
    
    tokens->deallocateStringElements();
    delete tokens;
}


int main( int inNumArgs, char **inArgs ) {
    
    if( inNumArgs != 4 ) {
        usage();
    }
    
    char *address = inArgs[1];
    
    int port = 8005;
    sscanf( inArgs[2], "%d", &port );
    
    char *emailPrefix = inArgs[3];
    
    // initialize an instance of Client
    Client *connection = new Client;
    
    // first, connect
    
    connection->i = 0;
    connection->id = 1;
    connection->skipCompressedData = 0;
    connection->pendingCMData = false;
    connection->moving = false;
    connection->dead = false;
    connection->disconnected = false;

    HostAddress a( stringDuplicate( address ), port );
        

    char timeout =  false;
    connection->sock = 
        SocketClient::connectToServer( &a, 5000, &timeout );

    if( timeout ) {
        printf( "Client timed out when trying to connect\n");
        delete connection->sock;
        connection->sock = NULL;
        return 1;
    }

    while(true) {
        if( connection->sock != NULL ) {
            
            char *email = autoSprintf( "%s@dummy.com", emailPrefix );

            printf( "Client connected, logging in with email %s\n",
                    email );

            
            char *message = autoSprintf( "LOGIN %s aaaa aaaa#",
                                            email );
            
            connection->sock->send( (unsigned char*)message, 
                                        strlen( message ), true, false );
            
            delete [] message;
            delete [] email;
        }
        else {
            printf( "Client failed to connect\n");
        }

        // process messages   
        if( connection->sock == NULL ) {
            printf("Client lost connection\n");
            return 1;
        }

        if( connection->id == -1 ) {
            // still waiting for first PU

            if( connection->disconnected ) {
                printf( "Client lost connection\n" );

                delete connection->sock;
                connection->sock = NULL;
            }

            char *message = getNextMessage(connection);

            if( message != NULL ) {
                //printf( "Client %d got message:\n%s\n\n", i, message );
                
                if( strstr( message, "MC" ) == message ) {
                    //printf( "Client %d got first map chunk\n", i );

                    int sizeX, sizeY, x, y, binarySize, compSize;
                    sscanf( message, "MC\n%d %d %d %d\n%d %d\n", 
                            &sizeX, &sizeY,
                            &x, &y, &binarySize, &compSize );
                
                    connection->skipCompressedData = compSize;
                }
                else if( strstr( message, "PU" ) == message ) {
                    // PU message!
                

                    // last line describes us

                    int numLines;
                    char **lines = split( message, "\n", &numLines );
                
                    if( numLines > 2 ) {
                    
                        // first line is about us
                        parsePlayerUpdateMessage(connection, lines[ numLines - 2 ] );
                    }
                
                    printf( "Client got first player update, "
                            "pid = %d, pos = %d,%d\n",
                            connection->id,
                            connection->x, 
                            connection->y );


                    
                    for( int p=0; p<numLines; p++ ) {
                        delete [] lines[p];
                    }
                    delete [] lines;
                }
                delete [] message;
            }                
        }
        else {
            // player is live
            
            if( connection->dead ) {
                printf( "Client died, closing connection\n");

                delete connection->sock;
                connection->sock = NULL;
                return 1;
            }
            if( connection->disconnected ) {
                printf( "Client lost connection\n");

                delete connection->sock;
                connection->sock = NULL;
                return 1;
            }

            if( !connection->moving ) {
            
                // make a move

                connection->moving = true;
            
                //printf("Client starting move\n");
                
                int xDelt = 0;
                int yDelt = 0;
                int lastXMove = 0;
                int lastYMove = 0;
                
                switch (lastXMove) {
                    case -1:
                        xDelt = 0;
                        yDelt = 1;
                        break;
                    
                    case 1:
                        xDelt = 0;
                        yDelt = -1;
                        break;
                    
                    default:
                        if (lastYMove == 1) {
                            xDelt = -1;
                            yDelt = 0;
                        } else {
                            xDelt = 1;
                            yDelt = 0;
                        }
                        break;
                }

                lastXMove = xDelt;
                lastYMove = yDelt;
            
                char *message = autoSprintf( "MOVE %d %d %d %d#",
                                                connection->x,
                                                connection->y,
                                                xDelt, yDelt );
            
                connection->sock->send( (unsigned char*)message, 
                                            strlen( message ), true, false );
            
                delete [] message;
            }

            char *message = getNextMessage(connection);

            if( message != NULL ) {
                if( strstr( message, "MC" ) == message ) {
                
                    int sizeX, sizeY, x, y, binarySize, compSize;
                    sscanf( message, "MC\n%d %d %d %d\n%d %d\n", 
                            &sizeX, &sizeY,
                            &x, &y, &binarySize, &compSize );
                
                    connection->skipCompressedData = compSize;
                }
                else if( strstr( message, "PU" ) == message ) {
                    // PU message!

                    int numLines;
                    char **lines = split( message, "\n", &numLines );
                
                    for( int p=1; p<numLines-1; p++ ) {
                        parsePlayerUpdateMessage(connection,
                                                    lines[p] );
                    }

                    for( int p=0; p<numLines; p++ ) {
                        delete [] lines[p];
                    }
                    delete [] lines;
                }
                delete [] message;
            }
        
        }
    }

    if( connection->sock != NULL ) {
        delete connection->sock;
    }    
    
    delete [] connection;
    
    return 1;
}