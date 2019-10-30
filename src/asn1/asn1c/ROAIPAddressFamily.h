/*
 * Generated by asn1c-0.9.29 (http://lionet.info/asn1c)
 * From ASN.1 module "RPKI-ROA"
 * 	found in "rfc6482.asn1"
 * 	`asn1c -Werror -fcompound-names -fwide-types -D asn1/asn1c -no-gen-PER -no-gen-example`
 */

#ifndef	_ROAIPAddressFamily_H_
#define	_ROAIPAddressFamily_H_


#include "asn1/asn1c/asn_application.h"

/* Including external dependencies */
#include "asn1/asn1c/OCTET_STRING.h"
#include "asn1/asn1c/asn_SEQUENCE_OF.h"
#include "asn1/asn1c/constr_SEQUENCE_OF.h"
#include "asn1/asn1c/constr_SEQUENCE.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct ROAIPAddress;

/* ROAIPAddressFamily */
typedef struct ROAIPAddressFamily {
	OCTET_STRING_t	 addressFamily;
	struct ROAIPAddressFamily__addresses {
		A_SEQUENCE_OF(struct ROAIPAddress) list;
		
		/* Context for parsing across buffer boundaries */
		asn_struct_ctx_t _asn_ctx;
	} addresses;
	
	/* Context for parsing across buffer boundaries */
	asn_struct_ctx_t _asn_ctx;
} ROAIPAddressFamily_t;

/* Implementation */
extern asn_TYPE_descriptor_t asn_DEF_ROAIPAddressFamily;
extern asn_SEQUENCE_specifics_t asn_SPC_ROAIPAddressFamily_specs_1;
extern asn_TYPE_member_t asn_MBR_ROAIPAddressFamily_1[2];

#ifdef __cplusplus
}
#endif

/* Referred external types */
#include "ROAIPAddress.h"

#endif	/* _ROAIPAddressFamily_H_ */
#include "asn1/asn1c/asn_internal.h"