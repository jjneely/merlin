#include "shared.h"

int proto_read_event(int sock, merlin_event *pkt)
{
	int len, result;

	len = io_recv_all(sock, &pkt->hdr, HDR_SIZE);
	if (len != HDR_SIZE) {
		lerr("In read_event: Incomplete header read(). Expected %zu, got %d",
			 HDR_SIZE, len);
		return -1;
	}

	if (pkt->hdr.protocol != MERLIN_PROTOCOL_VERSION) {
		lerr("Bad protocol version (%d, expected %d)\n",
			 pkt->hdr.protocol, MERLIN_PROTOCOL_VERSION);
		return -1;
	}

	if (!pkt->hdr.len && pkt->hdr.type != CTRL_PACKET) {
		lerr("Non-control packet of type %d with zero size length (this should never happen)", pkt->hdr.type);
		return len;
	}

	if (!pkt->hdr.len)
		return HDR_SIZE;

	result = io_recv_all(sock, pkt->body, pkt->hdr.len);
	if (result != pkt->hdr.len) {
		lwarn("Bogus read in proto_read_event(). got %d, expected %d",
			  result, pkt->hdr.len);
	}
	else {
		ldebug("Successfully read 1 %s event (%zu bytes; %u bytes body) from socket %d\n",
			   callback_name(pkt->hdr.type), HDR_SIZE + result, pkt->hdr.len, sock);
	}

	return result;
}

int proto_send_event(int sock, merlin_event *pkt)
{
	pkt->hdr.protocol = MERLIN_PROTOCOL_VERSION;

	if (pkt->hdr.len < 0 || packet_size(pkt) > TOTAL_PKT_SIZE) {
		ldebug("header is invalid, or packet is too large. aborting\n");
		return -1;
	}

	return io_send_all(sock, pkt, packet_size(pkt));
}

int proto_ctrl(int sock, int control_type, int selection)
{
	merlin_header hdr;

	memset(&hdr, 0, HDR_SIZE);

	hdr.type = CTRL_PACKET;
	hdr.len = 0;
	hdr.code = control_type;
	hdr.selection = selection;

	return io_send_all(sock, &hdr, HDR_SIZE);
}
