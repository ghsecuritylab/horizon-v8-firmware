/* syshal_cellular.h - HAL for cellular device
 *
 * Copyright (C) 2019 cellular
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "at.h"
#include "syshal_gpio.h"
#include "syshal_uart.h"
#include "syshal_cellular.h"
#include "syshal_time.h"
#include "syshal_rtc.h"
#include "debug.h"

#include <stdio.h>
#include <ctype.h>

#define POWER_UP_TIMEOUT_S (10) // The amount of time which after if the device is unresponsive communication attempts stop

static const uint8_t AT[] = "AT";
static const uint8_t OK[] = "OK";
static const uint8_t OK_HTTP[] = {'O', 'K', '\r', '\n', '\0'};
static const uint8_t DIS_ERR_VERB[] = "ATE0";
static const uint8_t ENABLE_VERB_ERR[] = "CMEE=1";
static const uint8_t DISABLE_UMWI[] = "UMWI=0";
static const uint8_t CIMI[] = "CIMI";
static const uint8_t CIMI_RESPONSE[] = "\r\n%s";
static const uint8_t clean_CPRF[] = "USECPRF=0";
static const uint8_t sec_level_CPRF[] = "USECPRF=0,0,0";
static const uint8_t mim_tls_CPRF[] = "USECPRF=0,1,3";
static const uint8_t root_CA_CPRF[] = "USECPRF=0,3,\"%s\"";
static const uint8_t device_cert_CPRF[] = "USECPRF=0,5,\"%s\"";
static const uint8_t device_key_CPRF[] = "USECPRF=0,6,\"%s\"";
static const uint8_t set_URAT[] = "URAT=%u";
static const uint8_t set_COPS5[] = "COPS=5";
static const uint8_t set_COPS0[] = "COPS=0";
static const uint8_t set_COPS2[] = "COPS=2";
static const uint8_t resp_COPS5[] = "MCC:%u";
static const uint8_t abort[] = "abort";
static const uint8_t aborted[] = "ABORTED";
static const uint8_t set_UPSD_APN[] = "UPSD=0,1,\"%s\"";
static const uint8_t set_UPSD_username[] = "UPSD=0,2,\"%s\"";
static const uint8_t set_UPSD_password[] = "UPSD=0,3,\"%s\"";
static const uint8_t activate_UPSD[] = "UPSDA=0,3";
static const uint8_t clean_HTTP0[] = "UHTTP=0";
static const uint8_t set_domain_HTTP0[] = "UHTTP=0,1,\"%s\"";
static const uint8_t set_sec_HTTP0[] = "UHTTP=0,6,1,0";
static const uint8_t set_port_HTTP0[] = "UHTTP=0,5,%u";
static const uint8_t get_HTTP0[] = "UHTTPC=0,1,\"%s\",\"TEMP.DAT\"";
static const uint8_t get_sucess[] = "+UUHTTPCR: 0,1,1";
static const uint8_t post_HTTP0[] = "UHTTPC=0,4,\"%s\",\"RESULT.DAT\",\"TEMP.DAT\",0";
static const uint8_t post_sucess[] = "+UUHTTPCR: 0,4,1";
static const uint8_t send_read_file[] = "URDFILE=\"TEMP.DAT\"";
static const uint8_t respond_read_file[] = "+URDFILE: \"TEMP.DAT\",%u,\"";
static const uint8_t delete_temp[] = "UDELFILE=\"TEMP.DAT\"";
static const uint8_t download_temp[] = "UDWNFILE=\"TEMP.DAT\",%u";
static const uint8_t start_data[] = ">";
static const char ROOT_CERT[] = "root-CA.pem";
static const char DEVICE_CERT[] = "deviceCert.pem";
static const char DEVICE_KEY[] = "deviceCert.key";

static enum
{
    STATE_ON,
    STATE_OFF,
    STATE_BOOTING,
    STATE_UNRESPONSIVE
} cellular_state;

static uint32_t powered_on_timestamp;

/*! \brief Translate AT ERROR IN SYSHAL cellular error.
 *
 * \return \ref SYSHAL_CELLULAR_NO_ERROR on success.
 * \return \ref SYSHAL_CELLULAR_ERROR_DEVICE error in fs,  UART or at module.
 * \return \ref SYSHAL_CELLULAR_ERROR_TIMEOUT timeout expired by UART module.
 * \return \ref SYSHAL_CELLULAR_UNEXPECTED_RESPONSE response isn't the expected one.
 */
static int error_mapping_at(int status)
{
    switch (status)
    {
        case AT_NO_ERROR:
            return SYSHAL_CELLULAR_NO_ERROR;
            break;
        case AT_ERROR_TIMEOUT:
            return SYSHAL_CELLULAR_ERROR_TIMEOUT;
            break;
        case AT_ERROR_UNEXPECTED_RESPONSE:
            return SYSHAL_CELLULAR_ERROR_UNEXPECTED_RESPONSE;
            break;
        case AT_ERROR_BUFFER_OVERFLOW:
            return SYSHAL_CELLULAR_ERROR_BUFFER_OVERFLOW;
            break;
        case AT_ERROR_HTTP:
            return SYSHAL_CELLULAR_ERROR_HTTP;
            break;
        default:
            return SYSHAL_CELLULAR_ERROR_DEVICE;
            break;
    }
}

static inline uint32_t get_size_command(const uint8_t *ptr)
{
    uint32_t len = 0;
    while (*ptr++ != '\0')
        len++;
    return len;
}

/*! \brief Determines if the device has successfully powered on and is responsive
 *
 * \return true on success
 * \return false on failure
 */
static bool has_device_booted(void)
{
    uint32_t current_time;
    uint32_t length_expect;
    int status;

#ifdef GTEST
    return true;
#endif

    switch (cellular_state)
    {
        case STATE_OFF:
            return false;
            break;
        case STATE_ON:
            return true;
            break;
        case STATE_UNRESPONSIVE:
            return false;
            break;
        case STATE_BOOTING:
            // The device is booting so we need to make sure it is responsive to AT commands
            do
            {
                if (at_flush())
                    return false;

                status = at_send_raw_with_cr(AT, get_size_command(AT));
                if (status != AT_NO_ERROR && status != AT_ERROR_TIMEOUT)
                    return false;

                if (status != AT_ERROR_TIMEOUT) // There is no point trying to read something if our send timed out
                {
                    status = at_expect(OK, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect);

                    if (status != AT_NO_ERROR && status != AT_ERROR_TIMEOUT)
                        return false;

                    if (status == AT_NO_ERROR)
                    {
                        cellular_state = STATE_ON;
                        return true;
                    }
                }

                syshal_time_delay_ms(100);

                syshal_rtc_get_timestamp(&current_time);

                // Kick the soft watchdog so we don't trigger it
                syshal_rtc_soft_watchdog_refresh();
            }
            while (current_time - powered_on_timestamp < POWER_UP_TIMEOUT_S);

            cellular_state = STATE_UNRESPONSIVE;
            return false;
            break;
        default:
            return false;
            break;
    }
}

/*! \brief Internal function for reading the header part of a file
 *
 *  Reading the first line until HTTP code, check http code. if http not equal 200
 * discard the rest of the file otherwise read the rest of the header and return the
 * HTTP code, the header length and the total length of the file
 *
 * \param timeout[in] UART timeout for network operation.
 * \param domain[in] UART domain for performing the  GET HTTP command.
 * \param port[in] UART port for performing the GET HTTP command.
 * \param path[in] UART path for performing the network operation.
 *
 * \return \ref SYSHAL_CELLULAR_NO_ERROR on success.
 * \return \ref SYSHAL_CELLULAR_ERROR_DEVICE error in fs,  UART or at module.
 * \return \ref SYSHAL_CELLULAR_ERROR_TIMEOUT timeout expired by UART module.
 * \return \ref SYSHAL_CELLULAR_UNEXPECTED_RESPONSE response isn't the expected one.
 * \return \ref SYSHAL_CELLULAR_ERROR_HTTP if a HTTP error was detected
 */
static int read_file_header(uint32_t *length_file, uint32_t *length_header, uint32_t *http_code)
{
    int status;

    uint32_t length_expect;
    uint32_t length_read;

    if (!has_device_booted())
        return SYSHAL_CELLULAR_ERROR_FAILED_TO_BOOT;

    /* Read LENGHT and save in file system*/
    status = error_mapping_at(at_flush());
    if (status)
        return status;

    status = error_mapping_at(at_send(send_read_file));
    if (status)
        return status;

    status = error_mapping_at(at_expect(respond_read_file, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect, length_file));
    if (status)
        return status;
    status = error_mapping_at(at_expect_http_header(&length_read, http_code));
    if (SYSHAL_CELLULAR_ERROR_HTTP == status)
    {
        status = error_mapping_at(at_discard(*length_file - length_read));
        if (status)
            return status;

        return SYSHAL_CELLULAR_ERROR_HTTP;
    }
    else if (status)
        return status;

    *length_header = length_read;

    return SYSHAL_CELLULAR_NO_ERROR;
}

/*! \brief Internal function for configuring HTTP profile
 *
 *  Configuring HTTP profile through AT command
 *  used in syshal_cellular_https_get() and syshal_cellular_https_post()
 *
 * \param timeout[in] UART timeout for network operation.
 * \param domain[in] UART domain for performing the  GET HTTP command.
 * \param port[in] UART port for performing the GET HTTP command.
 * \param path[in] UART path for performing the network operation.
 *
 * \return \ref SYSHAL_CELLULAR_NO_ERROR on success.
 * \return \ref SYSHAL_CELLULAR_ERROR_DEVICE error in fs,  UART or at module.
 * \return \ref SYSHAL_CELLULAR_ERROR_TIMEOUT timeout expired by UART module.
 * \return \ref SYSHAL_CELLULAR_UNEXPECTED_RESPONSE response isn't the expected one.
 */
static int configure_HTTP(const char *domain, uint32_t port, const char *path)
{
    int status;
    uint32_t length_expect;

    if (!has_device_booted())
        return SYSHAL_CELLULAR_ERROR_FAILED_TO_BOOT;

    /* Clean HTTP profile 0 */
    status = error_mapping_at(at_flush());
    if (status)
        return status;
    status = error_mapping_at(at_send(clean_HTTP0));
    if (status)
        return status;
    status = error_mapping_at(at_expect(OK, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
    if (status)
        return status;
    /* Set Domain for HTTP profile 0 */
    status = error_mapping_at(at_flush());
    if (status)
        return status;
    status = error_mapping_at(at_send(set_domain_HTTP0, domain));
    if (status)
        return status;
    status = error_mapping_at(at_expect(OK, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
    if (status)
        return status;

    status = error_mapping_at(at_flush());
    if (status)
        return status;
    /* Connect Secure profile 0 */
    status = error_mapping_at(at_send(set_sec_HTTP0));
    if (status)
        return status;
    status = error_mapping_at(at_expect(OK, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
    if (status)
        return status;

    status = error_mapping_at(at_flush());
    if (status)
        return status;
    /* Set port for HTTP profile 0 */
    status = error_mapping_at(at_send(set_port_HTTP0, port));
    if (status)
        return status;
    status = error_mapping_at(at_expect(OK, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
    if (status)
        return status;

    return SYSHAL_CELLULAR_NO_ERROR;
}

/*! \brief Initialise the cellular layer
 *
 * \return \ref SYSHAL_CELLULAR_NO_ERROR on success.
 */
int syshal_cellular_init(void)
{
    syshal_gpio_init(SYSHAL_CELLULAR_GPIO_POWER_ON);
    syshal_gpio_set_output_low(SYSHAL_CELLULAR_GPIO_POWER_ON);

    cellular_state = STATE_OFF;
    powered_on_timestamp = 0;

    if (syshal_uart_change_baud(UART_CELLULAR, UART_CELLULAR_BAUDRATE) != SYSHAL_UART_NO_ERROR)
        return SYSHAL_CELLULAR_ERROR_DEVICE;

    /* Configure AT Module */
    int status = error_mapping_at(at_init(UART_CELLULAR));
    if (status)
        return status;

    return SYSHAL_CELLULAR_NO_ERROR;
}

/*! \brief Power on SARA module
 *
 *  Power on module by setting high GPIO port
 *
 * \return \ref SYSHAL_CELLULAR_NO_ERROR on success.
 */
int syshal_cellular_power_on(void)
{
    syshal_gpio_set_output_high(SYSHAL_CELLULAR_GPIO_POWER_ON);

    if (STATE_ON != cellular_state)
    {
        syshal_rtc_get_timestamp(&powered_on_timestamp);
        cellular_state = STATE_BOOTING;
    }

    return SYSHAL_CELLULAR_NO_ERROR;
}

/*! \brief Init comms configuration
 *
 *  Init UART interface, set baudrate to UART, init AT module
 *  and check if the device is ready.
 *
 * \return \ref SYSHAL_CELLULAR_NO_ERROR on success.
 * \return \ref SYSHAL_CELLULAR_ERROR_DEVICE error in fs,  UART or at module.
 * \return \ref SYSHAL_CELLULAR_ERROR_TIMEOUT timeout expired by UART module.
 * \return \ref SYSHAL_CELLULAR_UNEXPECTED_RESPONSE response isn't the expected one.
 */
int syshal_cellular_sync_comms(void)
{
    int status;
    uint32_t length_expect;

    if (!has_device_booted())
        return SYSHAL_CELLULAR_ERROR_FAILED_TO_BOOT;

    status = error_mapping_at(at_flush());
    if (status)
        return status;

    status = error_mapping_at(at_send_raw_with_cr(DIS_ERR_VERB, get_size_command(DIS_ERR_VERB)));
    if (status)
        return status;

    status = error_mapping_at(at_expect(OK, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
    if (status)
        return status;

    status = error_mapping_at(at_flush());
    if (status)
        return status;

    status = error_mapping_at(at_send(DISABLE_UMWI));
    if (status)
        return status;

    status = error_mapping_at(at_expect(OK, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
    if (status)
        return status;

    status = error_mapping_at(at_flush());
    if (status)
        return status;

    status = error_mapping_at(at_send(ENABLE_VERB_ERR));
    if (status)
        return status;

    status = error_mapping_at(at_expect(OK, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
    if (status)
        return status;

    return SYSHAL_CELLULAR_NO_ERROR;
}

/*! \brief Power off SARA U270
 *
 *  Power off SARA U270 by using the AT+CPWROFF command.
 *
 * \return \ref SYSHAL_CELLULAR_NO_ERROR on success.
 * \return \ref SYSHAL_CELLULAR_ERROR_DEVICE error in fs,  UART or at module.
 * \return \ref SYSHAL_CELLULAR_ERROR_TIMEOUT timeout expired by UART module.
  * \return \ref SYSHAL_CELLULAR_UNEXPECTED_RESPONSE response isn't the expected one.
 */
int syshal_cellular_power_off(void)
{
    cellular_state = STATE_OFF;

    syshal_gpio_set_output_low(SYSHAL_CELLULAR_GPIO_POWER_ON);

    return SYSHAL_CELLULAR_NO_ERROR;
}

/*! \brief Check sim SARA U270
 *
 *  Command sequence for getting the IMSI from the SARA U270 device
 *
 * \param imsi[out] imsi sent by SARA U270.
 * \return \ref SYSHAL_CELLULAR_NO_ERROR on success.
 * \return \ref SYSHAL_CELLULAR_ERROR_DEVICE error in fs,  UART or at module.
 * \return \ref SYSHAL_CELLULAR_ERROR_TIMEOUT timeout expired by UART module.
  * \return \ref SYSHAL_CELLULAR_UNEXPECTED_RESPONSE response isn't the expected one.
 */
int syshal_cellular_check_sim(uint8_t *imsi)
{
    int status;
    uint32_t length_expect;
    uint32_t retries = 20;

    if (!has_device_booted())
        return SYSHAL_CELLULAR_ERROR_FAILED_TO_BOOT;

    // It has been observed that if the SIM card has not booted the CIMI returns as all zeros
    // because of this we should read until it does not

    while (retries)
    {
        status = error_mapping_at(at_flush());
        if (status)
            return status;

        status = error_mapping_at(at_send(CIMI));
        if (status)
            return status;

        status = error_mapping_at(at_expect(CIMI_RESPONSE, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect, (char*)imsi, 15));
        if (status == SYSHAL_CELLULAR_ERROR_TIMEOUT)
        {
            syshal_time_delay_ms(100);
            retries--;
            continue;
        }
        else if (status)
            return status;

        // If the IMSI contains characters that are not digits then it likely is an error so retry
        bool imsi_valid = true;
        for (uint32_t i = 0; i < 15; ++i)
        {
            if (!isdigit(imsi[i]))
            {
                imsi_valid = false;
                break;
            }
        }

        if (!imsi_valid)
        {
            syshal_time_delay_ms(100);
            retries--;
            continue;
        }

        status = error_mapping_at(at_expect(OK, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
        if (status)
            return status;

        for (uint32_t i = 0; i < 15; ++i)
            if (imsi[i] != '0')
                return SYSHAL_CELLULAR_NO_ERROR;

        // Ensure it is null terminated
        imsi[15] = '\0';

        retries--;

        syshal_time_delay_ms(100);
    }

    return SYSHAL_CELLULAR_ERROR_TIMEOUT;
}

/*! \brief Create a secure profile 0 for SARA U270
 *
 *  Command sequence for setting the secure profile in SARA U270
 *  Set the level of security - No certificate validation
 *  Set minimum TLS version -  (TLS 1.2)
 *  Assign root certificate, device certificate and device private key
 *
 * \return \ref SYSHAL_CELLULAR_NO_ERROR on success.
 * \return \ref SYSHAL_CELLULAR_ERROR_DEVICE error in fs,  UART or at module.
 * \return \ref SYSHAL_CELLULAR_ERROR_TIMEOUT timeout expired by UART module.
  * \return \ref SYSHAL_CELLULAR_UNEXPECTED_RESPONSE response isn't the expected one.
 */
int syshal_cellular_create_secure_profile(void)
{
    int status;
    uint32_t length_expect;

    if (!has_device_booted())
        return SYSHAL_CELLULAR_ERROR_FAILED_TO_BOOT;

    /* Clear secure profile 0 */
    status = error_mapping_at(at_flush());
    if (status)
        return status;
    status = error_mapping_at(at_send(clean_CPRF));
    if (status)
        return status;

    status = error_mapping_at(at_expect(OK, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
    if (status)
        return status;

    /* Set secure profile 0  security level 0 (No certificate validation)*/
    status = error_mapping_at(at_flush());
    if (status)
        return status;
    status = error_mapping_at(at_send(sec_level_CPRF));
    if (status)
        return status;
    status = error_mapping_at(at_expect(OK, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
    if (status)
        return status;

    /* Set secure profile 0 minimum STL/TLS version 0 (TLS 1.2)*/
    status = error_mapping_at(at_flush());
    if (status)
        return status;
    status = error_mapping_at(at_send(mim_tls_CPRF));
    if (status)
        return status;
    status = error_mapping_at(at_expect(OK, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
    if (status)
        return status;

    /* Select "root-CA.crt as device root certificate in secure profile 0")*/
    status = error_mapping_at(at_flush());
    if (status)
        return status;
    status = error_mapping_at(at_send(root_CA_CPRF, ROOT_CERT));
    if (status)
        return status;
    status = error_mapping_at(at_expect(OK, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
    if (status)
        return status;

    /* Select "deviceCert.pem" as device device certificate in secure profile 0")*/
    status = error_mapping_at(at_flush());
    if (status)
        return status;
    status = error_mapping_at(at_send(device_cert_CPRF, DEVICE_CERT));
    if (status)
        return status;
    status = error_mapping_at(at_expect(OK, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
    if (status)
        return status;

    /* Select "deviceCert.key" as device private key in secure profile 0")*/
    status = error_mapping_at(at_flush());
    if (status)
        return status;
    status = error_mapping_at(at_send(device_key_CPRF, DEVICE_KEY));
    if (status)
        return status;
    status = error_mapping_at(at_expect(OK, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
    if (status)
        return status;

    return SYSHAL_CELLULAR_NO_ERROR;
}

/*! \brief Set radio access technology
 *
 *  Command sequence for setting a specific radio access technology
 * Supported     SCAN_MODE_2G, SCAN_MODE_AUTO, SCAN_MODE_3G,
 *
 * \param timeout[in] UART timeout for network operation.
 * \param mode[in] scan mode desired
 * \return \ref SYSHAL_CELLULAR_NO_ERROR on success.
 * \return \ref SYSHAL_CELLULAR_ERROR_DEVICE error in fs,  UART or at module.
 * \return \ref SYSHAL_CELLULAR_ERROR_TIMEOUT timeout expired by UART module.
 * \return \ref SYSHAL_CELLULAR_UNEXPECTED_RESPONSE response isn't the expected one.
 */
int syshal_cellular_set_rat(uint32_t timeout_ms, scan_mode_t mode)
{
    int status;
    uint32_t length_expect;

    if (!has_device_booted())
        return SYSHAL_CELLULAR_ERROR_FAILED_TO_BOOT;

    /* Set RAT technology */
    status = error_mapping_at(at_flush());
    if (status)
        return status;

    status = error_mapping_at(at_send(set_URAT, mode));
    if (status)
        return status;

    status = error_mapping_at(at_expect(OK, timeout_ms, &length_expect));
    if (status)
        return status;

    return SYSHAL_CELLULAR_NO_ERROR;
}

/*! \brief run a cellular scan for SARA U270
 *
 *  Command sequence for chacking whether there are any cellular near or not.
 *  perform a extender network scan and stopping the operation when one near cell is found.
 * After the fist cell received the module send an abort signal in order to stop the searching
 *
 * \param timeout[in] UART timeout for network operation.
 * \return \ref SYSHAL_CELLULAR_NO_ERROR on success.
 * \return \ref SYSHAL_CELLULAR_ERROR_DEVICE error in fs,  UART or at module.
 * \return \ref SYSHAL_CELLULAR_ERROR_TIMEOUT timeout expired by UART module.
 * \return \ref SYSHAL_CELLULAR_UNEXPECTED_RESPONSE response isn't the expected one.
 */
int syshal_cellular_scan(uint32_t timeout_ms)
{
    uint32_t value;
    uint32_t length_expect;
    int status;

    if (!has_device_booted())
        return SYSHAL_CELLULAR_ERROR_FAILED_TO_BOOT;

    syshal_time_delay_ms(1000);

    /* Set extended network search */
    status = error_mapping_at(at_flush());
    if (status)
        return status;

    status = error_mapping_at(at_send(set_COPS5));
    if (status)
        return status;

    /* Receive first Line and abort */
    status = error_mapping_at(at_expect(resp_COPS5, timeout_ms, &length_expect, &value));
    if (status == SYSHAL_CELLULAR_ERROR_TIMEOUT)
    {
        at_send_raw_with_cr(abort, get_size_command(abort));
        return SYSHAL_CELLULAR_ERROR_TIMEOUT;
    }
    else if (status != SYSHAL_CELLULAR_NO_ERROR)
    {
        return status;
    }

    // Wait for the full response from the AT+COPS=5 to enter our buffer before flushing it. NGPT-320
    syshal_time_delay_ms(50);

    status = error_mapping_at(at_flush());
    if (status)
        return status;

    status = error_mapping_at(at_send_raw_with_cr(abort, get_size_command(abort)));
    if (status)
        return status;
    status = error_mapping_at(at_expect(aborted, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
    if (status)
        return status;

    return SYSHAL_CELLULAR_NO_ERROR;
}

/*! \brief run a cellular attach for SARA U270
 *
 *  Command sequence for attaching to the network, then an
 * automatic network connection is sent.
 *
 * \param timeout[in] UART timeout for network operation.
 * \return \ref SYSHAL_CELLULAR_NO_ERROR on success.
 * \return \ref SYSHAL_CELLULAR_ERROR_DEVICE error in fs,  UART or at module.
 * \return \ref SYSHAL_CELLULAR_ERROR_TIMEOUT timeout expired by UART module.
 * \return \ref SYSHAL_CELLULAR_UNEXPECTED_RESPONSE response isn't the expected one.
 */
int syshal_cellular_attach(uint32_t timeout_ms)
{
    int status;
    uint32_t length_expect;

    if (!has_device_booted())
        return SYSHAL_CELLULAR_ERROR_FAILED_TO_BOOT;

    /* Set AutoConnection to the network */
    status = error_mapping_at(at_flush());
    if (status)
        return status;

    status = error_mapping_at(at_send(set_COPS0));
    if (status)
        return status;

    status = error_mapping_at(at_expect(OK, timeout_ms, &length_expect));
    if (status)
        return status;

    return SYSHAL_CELLULAR_NO_ERROR;
}

/*! \brief run a cellular dettach for SARA U270
 *
 *  Command sequence for dettaching from the network,
 *
 * \param timeout[in] UART timeout for network operation.
 * \return \ref SYSHAL_CELLULAR_NO_ERROR on success.
 * \return \ref SYSHAL_CELLULAR_ERROR_DEVICE error in fs,  UART or at module.
 * \return \ref SYSHAL_CELLULAR_ERROR_TIMEOUT timeout expired by UART module.
 * \return \ref SYSHAL_CELLULAR_UNEXPECTED_RESPONSE response isn't the expected one.
 */
int syshal_cellular_dettach(uint32_t timeout_ms)
{
    int status;
    uint32_t length_expect;

    if (!has_device_booted())
        return SYSHAL_CELLULAR_ERROR_FAILED_TO_BOOT;

    /* Set AutoConnection to the network */
    status = error_mapping_at(at_flush());
    if (status)
        return status;

    status = error_mapping_at(at_send(set_COPS2));
    if (status)
        return status;

    status = error_mapping_at(at_expect(OK, timeout_ms, &length_expect));
    if (status)
        return status;

    return SYSHAL_CELLULAR_NO_ERROR;
}

/*! \brief Create and activate a PSD and PDP context
 *
 *  Command sequence for creating a Packet Switch Data context, activate it,
 *  creating a packet data context and activate it.
 *  Packet switch data context is for the internal network connection
 *  Packet data protocol context is for the external network connection
 *
 * \param timeout[in] UART timeout for network operation.
 * \return \ref SYSHAL_CELLULAR_NO_ERROR on success.
 * \return \ref SYSHAL_CELLULAR_ERROR_DEVICE error in fs,  UART or at module.
 * \return \ref SYSHAL_CELLULAR_ERROR_TIMEOUT timeout expired by UART module.
 * \return \ref SYSHAL_CELLULAR_UNEXPECTED_RESPONSE response isn't the expected one.
 */
int syshal_cellular_activate_pdp(syshal_cellular_apn_t *apn, uint32_t timeout_ms)
{
    int status;
    uint32_t length_expect;

    if (!has_device_booted())
        return SYSHAL_CELLULAR_ERROR_FAILED_TO_BOOT;

    /* Set Packet data profile 1 with APN apn.name*/
    status = error_mapping_at(at_flush());
    if (status)
        return status;
    status = error_mapping_at(at_send(set_UPSD_APN, apn->name));
    if (status)
        return status;
    status = error_mapping_at(at_expect(OK, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
    if (status)
        return status;
    // username-password  required mode
    if (apn->username[0] != '\0')
    {
        /* Set Packet data profile 1 with username apn.username*/
        status = error_mapping_at(at_flush());
        if (status)
            return status;
        status = error_mapping_at(at_send(set_UPSD_username, apn->username));
        if (status)
            return status;
        status = error_mapping_at(at_expect(OK, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
        if (status)
            return status;

        /* Set Packet data profile 1 with APN apn.password*/
        status = error_mapping_at(at_flush());
        if (status)
            return status;
        status = error_mapping_at(at_send(set_UPSD_password, apn->password));
        if (status)
            return status;
        status = error_mapping_at(at_expect(OK, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
        if (status)
            return status;

    }
    /* Activate Packet switch data Context */
    status = error_mapping_at(at_flush());
    if (status)
        return status;
    status = error_mapping_at(at_send(activate_UPSD));
    if (status)
        return status;
    status = error_mapping_at(at_expect(OK, timeout_ms, &length_expect));
    if (status)
        return status;

    return SYSHAL_CELLULAR_NO_ERROR;
}

/*! \brief Perform a HTTPS command over SARA U270
 *
 *  Sequence for creating the HTTP profile, send a GET command and
 *  Store the result in TEMP.DAT
 *
 * \param timeout[in] UART timeout for network operation.
 * \param domain[in] UART domain for performing the  GET HTTP command.
 * \param port[in] UART port for performing the GET HTTP command.
 * \param path[in] UART path for performing the network operation.
 *
 * \return \ref SYSHAL_CELLULAR_NO_ERROR on success.
 * \return \ref SYSHAL_CELLULAR_ERROR_DEVICE error in fs,  UART or at module.
 * \return \ref SYSHAL_CELLULAR_ERROR_TIMEOUT timeout expired by UART module.
 * \return \ref SYSHAL_CELLULAR_UNEXPECTED_RESPONSE response isn't the expected one.
 */
int syshal_cellular_https_get(uint32_t timeout_ms, const char *domain, uint32_t port, const char *path)
{
    int status;
    uint32_t length_expect;

    if (!has_device_booted())
        return SYSHAL_CELLULAR_ERROR_FAILED_TO_BOOT;

    status = configure_HTTP(domain, port, path);
    if (status)
        return status;

    status = error_mapping_at(at_flush());
    if (status)
        return status;

    /* Perform GET HTTP command in HTTP profile 0 */
    status = error_mapping_at(at_send(get_HTTP0, path));
    if (status)
        return status;
    status = error_mapping_at(at_expect(OK_HTTP, timeout_ms, &length_expect));
    if (status)
        return status;

    status = error_mapping_at(at_expect(get_sucess, timeout_ms, &length_expect));
    if (status)
        return status;
    return SYSHAL_CELLULAR_NO_ERROR;
}

/*! \brief Perform a HTTPS POST command over SARA U270
 *
 *  Sequence for creating the HTTP profile, send a POST with the content of
 *  TEMP.DAT and save the response in RESULT.DAT
 *
 * \param timeout[in] UART timeout for network operation.
 * \param domain[in] UART domain for performing the  GET HTTP command.
 * \param port[in] UART port for performing the GET HTTP command.
 * \param path[in] UART path for performing the network operation.
 *
 * \return \ref SYSHAL_CELLULAR_NO_ERROR on success.
 * \return \ref SYSHAL_CELLULAR_ERROR_DEVICE error in fs,  UART or at module.
 * \return \ref SYSHAL_CELLULAR_ERROR_TIMEOUT timeout expired by UART module.
 * \return \ref SYSHAL_CELLULAR_UNEXPECTED_RESPONSE response isn't the expected one.
 */
int syshal_cellular_https_post(uint32_t timeout_ms, const char *domain, uint32_t port, const char *path)
{
    int status;
    uint32_t length_expect;

    if (!has_device_booted())
        return SYSHAL_CELLULAR_ERROR_FAILED_TO_BOOT;

    status = configure_HTTP(domain, port, path);
    if (status)
        return status;

    status = error_mapping_at(at_flush());
    if (status)
        return status;

    /* Perform POST HTTP command in HTTP profile 0 */
    status = error_mapping_at(at_send(post_HTTP0, path));
    if (status)
        return status;
    status = error_mapping_at(at_expect(OK_HTTP, timeout_ms, &length_expect));
    if (status)
        return status;
    status = error_mapping_at(at_expect(post_sucess, timeout_ms, &length_expect));
    if (status)
        return status;

    return SYSHAL_CELLULAR_NO_ERROR;
}

/*! \brief Read TEMP.DATA from SARA U270 to FS
 *
 *  Sequence for requesting the content of TEMP.DAT from SARA U270
 * and store it n the file sysmtem using handle.
 *
 * \param handle[in] file handle to write in.
 * \return \ref SYSHAL_CELLULAR_NO_ERROR on success.
 * \return \ref SYSHAL_CELLULAR_ERROR_DEVICE error in fs,  UART or at module.
 * \return \ref SYSHAL_CELLULAR_ERROR_TIMEOUT timeout expired by UART module.
 * \return \ref SYSHAL_CELLULAR_UNEXPECTED_RESPONSE response isn't the expected one.
 */
int syshal_cellular_read_from_file_to_fs(fs_handle_t handle, uint32_t *http_code, uint32_t *file_size)
{
    int status;
    uint32_t length_header, length_file;

    if (!has_device_booted())
        return SYSHAL_CELLULAR_ERROR_FAILED_TO_BOOT;

    status = read_file_header(&length_file, &length_header, http_code);
    if (status)
        return status;

    if (file_size)
        *file_size = length_file - length_header;

    status = error_mapping_at(at_read_raw_to_fs(SYSHAL_CELLULAR_TIMEOUT_MS, length_file - length_header, handle));
    if (status)
        return status;

    return SYSHAL_CELLULAR_NO_ERROR;
}

/*! \brief Read TEMP.DATA from SARA U270 to buffer
 *
 *  Sequence for requesting the content of TEMP.DAT from SARA U270
 * and store it in a buffer passed as parameter.
 *
 * \param buffer[in] buffer for storing the content of the file.
 * \param buffer_size[in] buffer_size maximum size of the buffer.
 * \return \ref SYSHAL_CELLULAR_NO_ERROR on success.
 * \return \ref SYSHAL_CELLULAR_ERROR_DEVICE error in fs,  UART or at module.
 * \return \ref SYSHAL_CELLULAR_ERROR_TIMEOUT timeout expired by UART module.
 * \return \ref SYSHAL_CELLULAR_UNEXPECTED_RESPONSE response isn't the expected one.
 * CREATE a common function for reading from file. UPGRADE
 */
int syshal_cellular_read_from_file_to_buffer(uint8_t *buffer, uint32_t buffer_size, uint32_t *bytes_written, uint32_t *http_code)
{
    int status;
    uint32_t length_header, length_file;

    if (!has_device_booted())
        return SYSHAL_CELLULAR_ERROR_FAILED_TO_BOOT;

    status = read_file_header(&length_file, &length_header, http_code);
    if (status)
        return status;

    if ((length_file - length_header) > buffer_size)
        return SYSHAL_CELLULAR_ERROR_BUFFER_OVERFLOW;

    status = error_mapping_at(at_read_raw_to_buffer(SYSHAL_CELLULAR_TIMEOUT_MS, length_file - length_header, buffer));
    if (status)
        return status;

    *bytes_written = length_file - length_header;

    return SYSHAL_CELLULAR_NO_ERROR;
}

/*! \brief Write from buffer to TEMP.DATA in SARA U270
 *
 *  Sequence for sending the content of the buffer to the temporal file
 *  TEMP file in the file system.
 *
 * \param buffer[in] buffer for storing the content of the file.
 * \param buffer_size[in] buffer_size maximum size of the buffer.
 * \return \ref SYSHAL_CELLULAR_NO_ERROR on success.
 * \return \ref SYSHAL_CELLULAR_ERROR_DEVICE error in fs,  UART or at module.
 * \return \ref SYSHAL_CELLULAR_ERROR_TIMEOUT timeout expired by UART module.
 * \return \ref SYSHAL_CELLULAR_UNEXPECTED_RESPONSE response isn't the expected one.
 */
int syshal_cellular_write_from_buffer_to_file(const uint8_t *buffer, uint32_t buffer_size)
{
    int status;
    uint32_t length_expect;

    if (!has_device_booted())
        return SYSHAL_CELLULAR_ERROR_FAILED_TO_BOOT;

    /* Delete file */
    status = error_mapping_at(at_flush());
    if (status)
        return status;

    /* Delete DATA.TEMP file */
    status = error_mapping_at(at_send(delete_temp));
    if (status == SYSHAL_CELLULAR_NO_ERROR)
    {
        status = error_mapping_at(at_expect(OK, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
        /* Don't do anything if it is received an unexpected response */
        if (status != SYSHAL_CELLULAR_NO_ERROR && status != SYSHAL_CELLULAR_ERROR_UNEXPECTED_RESPONSE)
            return status;
    }
    else
    {
        return status;
    }
    /* Send download command*/
    status = error_mapping_at(at_flush());
    if (status)
        return status;
    status = error_mapping_at(at_send(download_temp, buffer_size));
    if (status)
        return status;
    status = error_mapping_at(at_expect(start_data, SYSHAL_CELLULAR_FILE_TIMEOUT_MS, &length_expect));
    if (status)
        return status;
    status = error_mapping_at(at_send_raw_with_cr(buffer, buffer_size));
    if (status)
        return status;
    status = error_mapping_at(at_expect(OK, SYSHAL_CELLULAR_FILE_TIMEOUT_MS, &length_expect));
    if (status)
        return status;

    return SYSHAL_CELLULAR_NO_ERROR;
}

/* NOT IN USE */
int syshal_cellular_write_from_fs_to_file(fs_handle_t handle, uint32_t length)
{
    DEBUG_PR_WARN("%s NOT TESTED!!", __FUNCTION__);

    int status;
    uint32_t length_expect;

    if (!has_device_booted())
        return SYSHAL_CELLULAR_ERROR_FAILED_TO_BOOT;

    /* Delete file */
    status = error_mapping_at(at_flush());
    if (status)
        return status;

    /* Delete DATA.TEMP file */
    status = error_mapping_at(at_send(delete_temp));
    if (status == SYSHAL_CELLULAR_NO_ERROR)
    {
        status = error_mapping_at(at_expect(OK, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
        /* Don't do anything if it is received an unexpected response */
        if (status != SYSHAL_CELLULAR_NO_ERROR && status != SYSHAL_CELLULAR_ERROR_UNEXPECTED_RESPONSE)
            return status;
    }
    else
    {
        return status;
    }

    /* Read LENGTH and save in file system */
    status = error_mapping_at(at_flush());
    if (status)
        return status;
    status = error_mapping_at(at_send(download_temp, length));
    if (status)
        return status;

    // WARN: NOT SURE
    status = error_mapping_at(at_expect(start_data, SYSHAL_CELLULAR_TIMEOUT_MS, &length_expect));
    if (status)
        return status;
    status = error_mapping_at(at_send_raw_fs(handle, length));
    if (status)
        return status;

    return SYSHAL_CELLULAR_NO_ERROR;
}

/**
 * @brief      Sends raw unedited data to the Cellular module
 *
 * @param[in]  data  The data to be transmitted
 * @param[in]  size  The size of the data in bytes
 *
 * @return     Error code
 */
int syshal_cellular_send_raw(uint8_t * data, uint32_t size)
{
    if (syshal_uart_send(UART_CELLULAR, data, size) != SYSHAL_UART_NO_ERROR)
        return SYSHAL_CELLULAR_ERROR_DEVICE;
    else
        return SYSHAL_CELLULAR_NO_ERROR;
}

/**
 * @brief      Receives raw unedited data to the Cellular module.
 *
 * @param[in]  data  The received data
 * @param[in]  size  The max size of the data to be read in bytes
 *
 * @return     Actual number of bytes read
 */
int syshal_cellular_receive_raw(uint8_t * data, uint32_t size)
{
    return syshal_uart_receive(UART_CELLULAR, data, size);
}

/**
 * @brief      Returns the number of bytes in the cellular receive buffer
 *
 * @return     Number of bytes
 */
uint32_t syshal_cellular_available_raw(void)
{
    return syshal_uart_available(UART_CELLULAR);
}

/**
 * @brief      performs commmand sequence for checking whether the device is
 *             present or not. After this function the device is powered on and
 *             synchronized. It could receive any commnad such us check sim
 *
 * @return     true if device detected
 * @return     false if device is not found
 */
bool syshal_cellular_is_present(void)
{
    if (syshal_cellular_power_on() != SYSHAL_CELLULAR_NO_ERROR)
        return false;

    // We can make it simpler just configuring AT sending AT Commnad and cheking the echo.
    if (syshal_cellular_sync_comms() != SYSHAL_CELLULAR_NO_ERROR)
        return false;

    return true;
}
