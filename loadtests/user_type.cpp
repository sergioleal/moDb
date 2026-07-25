#include "user_type.hpp"

namespace modb::loadtest {

object::BindingBuilder<User> user_binding() {
    object::BindingBuilder<User> builder{"User"};
    builder.field<1>("id", &User::id)
        .field<2>("login", &User::login)
        .field<3>("email", &User::email)
        .field<4>("display_name", &User::display_name)
        .field<5>("created_at", &User::created_at)
        .field<6>("status", &User::status)
        .field<7>("filler", &User::filler);
    return builder;
}

User to_engine_user(const GeneratedUser& g) {
    User u;
    u.id = g.id;
    u.login = g.login;
    u.email = g.email;
    u.display_name = g.display_name;
    u.created_at = g.created_at;
    u.status = g.status;
    u.filler = g.filler;
    return u;
}

GeneratedUser from_engine_user(const User& u) {
    GeneratedUser g;
    g.id = u.id;
    g.login = u.login;
    g.email = u.email;
    g.display_name = u.display_name;
    g.created_at = u.created_at;
    g.status = u.status;
    g.filler = u.filler;
    return g;
}

} // namespace modb::loadtest
