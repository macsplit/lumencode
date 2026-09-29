package app.rest;

import javax.ws.rs.*;

@Path("/users")
public class UserResource {

    @GET
    public String all() {
        return "[]";
    }

    @GET
    @Path("{id}")
    public String one(@PathParam("id") String id) {
        return id;
    }
}
