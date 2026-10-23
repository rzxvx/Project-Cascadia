failed:
	if (c->touched || c->memberships[owner]) {
		c->fault = true;
		c->fault_error = ret;
	}
	return ret;
}
