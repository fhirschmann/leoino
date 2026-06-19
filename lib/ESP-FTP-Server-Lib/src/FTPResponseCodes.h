#ifndef FTP_RESPONSE_CODES_H_
#define FTP_RESPONSE_CODES_H_

// FTP Response Code Constants
namespace FtpCodes {
// Success codes
const int DATA_CONNECTION_OPEN = 150;
const int COMMAND_OK           = 200;
const int SYSTEM_STATUS        = 211;
const int DIRECTORY_STATUS     = 212;
const int FILE_STATUS          = 213;
const int HELP_MESSAGE         = 214;
const int SYSTEM_TYPE          = 215;
const int READY                = 220;
const int CLOSING              = 221;
const int TRANSFER_COMPLETE    = 226;
const int ENTERING_PASV_MODE   = 227;
const int LOGGED_IN            = 230;
const int FILE_ACTION_OK       = 250;
const int PATHNAME_CREATED     = 257;
const int USER_OK              = 331;
const int NEED_PASSWORD        = 331;
const int FILE_ACTION_PENDING  = 350;

// Error codes
const int SYNTAX_ERROR                          = 500;
const int SYNTAX_ERROR_PARAMS                   = 501;
const int COMMAND_NOT_IMPLEMENTED               = 502;
const int BAD_SEQUENCE                          = 503;
const int COMMAND_NOT_IMPLEMENTED_FOR_PARAMETER = 504;
const int NOT_LOGGED_IN                         = 530;
const int NEED_ACCOUNT                          = 532;
const int FILE_ACTION_NOT_TAKEN                 = 550;
const int FILE_NOT_FOUND                        = 550;
const int PAGE_TYPE_UNKNOWN                     = 551;
const int EXCEEDED_STORAGE                      = 552;
const int FILE_NAME_NOT_ALLOWED                 = 553;
const int TRANSFER_ABORTED                      = 426;
const int NO_DATA_CONNECTION                    = 425;
const int CANNOT_OPEN_CONNECTION                = 425;
const int CONNECTION_CLOSED                     = 426;
const int FILE_ACTION_ABORTED                   = 450;
const int FILE_ACTION_ABORTED_LOCAL_ERROR       = 451;
const int INSUFFICIENT_STORAGE                  = 452;
} // namespace FtpCodes

#endif
